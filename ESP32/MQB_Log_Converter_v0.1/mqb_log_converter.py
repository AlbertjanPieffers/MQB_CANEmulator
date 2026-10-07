from __future__ import annotations

import argparse
import csv
import json
import os
import struct
import sys
import tkinter as tk
from dataclasses import dataclass
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from typing import BinaryIO, Iterator


APP_NAME = "MQB Log Converter"
APP_VERSION = "0.1.0"

MQBLOG_MAGIC = 0x474C514D
MQBLOG_VERSION = 1

HEADER_STRUCT = struct.Struct("<IHHII")
RECORD_STRUCT = struct.Struct("<QIBBBB8s")

HEADER_SIZE = HEADER_STRUCT.size
RECORD_SIZE = RECORD_STRUCT.size

RECORD_TYPE_CAN = 1
RECORD_TYPE_MARKER = 2

FLAG_EXTENDED = 0x01


@dataclass
class LogHeader:
    magic: int
    format_version: int
    header_size: int
    bitrate: int
    reserved: int


@dataclass
class LogRecord:
    timestamp_us: int
    can_id: int
    record_type: int
    step: int
    flags: int
    dlc: int
    data: bytes

    @property
    def is_can(self) -> bool:
        return self.record_type == RECORD_TYPE_CAN

    @property
    def is_marker(self) -> bool:
        return self.record_type == RECORD_TYPE_MARKER

    @property
    def is_extended(self) -> bool:
        return bool(self.flags & FLAG_EXTENDED)

    @property
    def payload(self) -> bytes:
        return self.data[: min(self.dlc, 8)]

    @property
    def marker_text(self) -> str:
        return self.payload.decode("utf-8", errors="replace").rstrip("\x00")


@dataclass
class LogStatistics:
    file_size: int = 0
    total_records: int = 0
    can_frames: int = 0
    markers: int = 0
    unknown_records: int = 0
    unique_ids: int = 0
    first_timestamp_us: int | None = None
    last_timestamp_us: int | None = None

    @property
    def duration_seconds(self) -> float:
        if self.first_timestamp_us is None or self.last_timestamp_us is None:
            return 0.0
        return max(
            0.0,
            (self.last_timestamp_us - self.first_timestamp_us) / 1_000_000.0,
        )


class MqbLogError(Exception):
    pass


class MqbLogReader:
    def __init__(self, path: Path | str) -> None:
        self.path = Path(path)
        self.header: LogHeader | None = None

    def read_header(self) -> LogHeader:
        try:
            with self.path.open("rb") as stream:
                raw = stream.read(HEADER_SIZE)
        except OSError as exc:
            raise MqbLogError(f"Could not open file: {exc}") from exc

        if len(raw) != HEADER_SIZE:
            raise MqbLogError(
                f"File is too small to contain an MQBLOG header "
                f"({len(raw)} of {HEADER_SIZE} bytes)."
            )

        header = LogHeader(*HEADER_STRUCT.unpack(raw))

        if header.magic != MQBLOG_MAGIC:
            raise MqbLogError(
                "Invalid MQBLOG magic value. "
                "This file does not appear to be an MQB Guided Logger file."
            )

        if header.format_version != MQBLOG_VERSION:
            raise MqbLogError(
                f"Unsupported MQBLOG version {header.format_version}. "
                f"This converter supports version {MQBLOG_VERSION}."
            )

        if header.header_size < HEADER_SIZE:
            raise MqbLogError(
                f"Invalid header size {header.header_size}."
            )

        self.header = header
        return header

    def records(self) -> Iterator[LogRecord]:
        header = self.header or self.read_header()

        with self.path.open("rb") as stream:
            stream.seek(header.header_size)

            while True:
                raw = stream.read(RECORD_SIZE)

                if not raw:
                    break

                if len(raw) != RECORD_SIZE:
                    raise MqbLogError(
                        f"Truncated record at file offset "
                        f"0x{stream.tell() - len(raw):X}: "
                        f"expected {RECORD_SIZE} bytes, got {len(raw)}."
                    )

                values = RECORD_STRUCT.unpack(raw)

                yield LogRecord(
                    timestamp_us=values[0],
                    can_id=values[1],
                    record_type=values[2],
                    step=values[3],
                    flags=values[4],
                    dlc=values[5],
                    data=values[6],
                )

    def statistics(self) -> LogStatistics:
        stats = LogStatistics(file_size=self.path.stat().st_size)
        ids: set[int] = set()

        for record in self.records():
            stats.total_records += 1

            if stats.first_timestamp_us is None:
                stats.first_timestamp_us = record.timestamp_us

            stats.last_timestamp_us = record.timestamp_us

            if record.is_can:
                stats.can_frames += 1
                ids.add(record.can_id)
            elif record.is_marker:
                stats.markers += 1
            else:
                stats.unknown_records += 1

        stats.unique_ids = len(ids)
        return stats


def bytes_to_hex_spaced(payload: bytes) -> str:
    return " ".join(f"{value:02X}" for value in payload)


def bytes_to_hex_compact(payload: bytes) -> str:
    return "".join(f"{value:02X}" for value in payload)


def can_id_text(record: LogRecord) -> str:
    width = 8 if record.is_extended else 3
    return f"{record.can_id:0{width}X}"


def export_csv(reader: MqbLogReader, output_path: Path) -> None:
    with output_path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(
            [
                "timestamp_us",
                "relative_s",
                "step",
                "event",
                "can_id",
                "format",
                "dlc",
                "data",
            ]
        )

        first_timestamp: int | None = None

        for record in reader.records():
            if first_timestamp is None:
                first_timestamp = record.timestamp_us

            relative_s = (
                record.timestamp_us - first_timestamp
            ) / 1_000_000.0

            if record.is_can:
                writer.writerow(
                    [
                        record.timestamp_us,
                        f"{relative_s:.6f}",
                        record.step,
                        "",
                        f"0x{can_id_text(record)}",
                        "EXT" if record.is_extended else "STD",
                        record.dlc,
                        bytes_to_hex_spaced(record.payload),
                    ]
                )
            elif record.is_marker:
                writer.writerow(
                    [
                        record.timestamp_us,
                        f"{relative_s:.6f}",
                        record.step,
                        record.marker_text,
                        "",
                        "",
                        "",
                        "",
                    ]
                )
            else:
                writer.writerow(
                    [
                        record.timestamp_us,
                        f"{relative_s:.6f}",
                        record.step,
                        f"UNKNOWN_TYPE_{record.record_type}",
                        "",
                        "",
                        record.dlc,
                        bytes_to_hex_spaced(record.payload),
                    ]
                )


def export_asc(reader: MqbLogReader, output_path: Path) -> None:
    with output_path.open("w", encoding="ascii", errors="replace") as output:
        header = reader.header or reader.read_header()

        output.write("base hex timestamps absolute\n")
        output.write("internal events logged\n")
        output.write(f"// MQB Log Converter {APP_VERSION}\n")
        output.write(f"// Source bitrate: {header.bitrate}\n")

        first_timestamp: int | None = None

        for record in reader.records():
            if first_timestamp is None:
                first_timestamp = record.timestamp_us

            relative_s = (
                record.timestamp_us - first_timestamp
            ) / 1_000_000.0

            if record.is_can:
                frame_format = "x" if record.is_extended else ""
                data_text = bytes_to_hex_spaced(record.payload)

                output.write(
                    f"{relative_s:.6f} 1 "
                    f"{can_id_text(record)}{frame_format} "
                    f"Rx d {record.dlc} {data_text}\n"
                )
            elif record.is_marker:
                output.write(
                    f"// {relative_s:.6f} STEP {record.step} "
                    f"MARKER {record.marker_text}\n"
                )


def export_candump(reader: MqbLogReader, output_path: Path) -> None:
    with output_path.open("w", encoding="ascii", errors="replace") as output:
        first_timestamp: int | None = None

        for record in reader.records():
            if first_timestamp is None:
                first_timestamp = record.timestamp_us

            relative_s = (
                record.timestamp_us - first_timestamp
            ) / 1_000_000.0

            if record.is_can:
                output.write(
                    f"({relative_s:.6f}) can0 "
                    f"{can_id_text(record)}#"
                    f"{bytes_to_hex_compact(record.payload)}\n"
                )
            elif record.is_marker:
                output.write(
                    f"# {relative_s:.6f} STEP {record.step} "
                    f"MARKER {record.marker_text}\n"
                )


def export_json(reader: MqbLogReader, output_path: Path) -> None:
    header = reader.header or reader.read_header()

    with output_path.open("w", encoding="utf-8") as output:
        output.write("{\n")
        output.write('  "format": "MQBLOG",\n')
        output.write(f'  "formatVersion": {header.format_version},\n')
        output.write(f'  "bitrate": {header.bitrate},\n')
        output.write('  "records": [\n')

        first = True
        first_timestamp: int | None = None

        for record in reader.records():
            if first_timestamp is None:
                first_timestamp = record.timestamp_us

            relative_s = (
                record.timestamp_us - first_timestamp
            ) / 1_000_000.0

            if not first:
                output.write(",\n")

            first = False

            if record.is_can:
                item = {
                    "timestampUs": record.timestamp_us,
                    "relativeSeconds": round(relative_s, 6),
                    "step": record.step,
                    "type": "can",
                    "canId": record.can_id,
                    "canIdHex": f"0x{can_id_text(record)}",
                    "extended": record.is_extended,
                    "dlc": record.dlc,
                    "data": list(record.payload),
                    "dataHex": bytes_to_hex_spaced(record.payload),
                }
            elif record.is_marker:
                item = {
                    "timestampUs": record.timestamp_us,
                    "relativeSeconds": round(relative_s, 6),
                    "step": record.step,
                    "type": "marker",
                    "text": record.marker_text,
                }
            else:
                item = {
                    "timestampUs": record.timestamp_us,
                    "relativeSeconds": round(relative_s, 6),
                    "step": record.step,
                    "type": "unknown",
                    "recordType": record.record_type,
                    "dlc": record.dlc,
                    "dataHex": bytes_to_hex_spaced(record.payload),
                }

            output.write(
                "    " + json.dumps(item, ensure_ascii=False)
            )

        output.write("\n  ]\n")
        output.write("}\n")


EXPORTERS = {
    "csv": export_csv,
    "asc": export_asc,
    "candump": export_candump,
    "json": export_json,
}


def default_output_path(input_path: Path, export_format: str) -> Path:
    extension = {
        "csv": ".csv",
        "asc": ".asc",
        "candump": ".log",
        "json": ".json",
    }[export_format]

    return input_path.with_suffix(extension)


def format_bytes(value: int) -> str:
    units = ["B", "KB", "MB", "GB"]
    amount = float(value)

    for unit in units:
        if amount < 1024.0 or unit == units[-1]:
            if unit == "B":
                return f"{int(amount)} {unit}"
            return f"{amount:.2f} {unit}"

        amount /= 1024.0

    return f"{value} B"


def format_duration(seconds: float) -> str:
    minutes, sec = divmod(seconds, 60.0)
    hours, minutes = divmod(int(minutes), 60)

    if hours > 0:
        return f"{hours:02d}:{minutes:02d}:{sec:06.3f}"

    return f"{minutes:02d}:{sec:06.3f}"


class ConverterGui:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title(f"{APP_NAME} v{APP_VERSION}")
        self.root.geometry("760x600")
        self.root.minsize(680, 540)

        self.current_path: Path | None = None
        self.reader: MqbLogReader | None = None

        self.path_var = tk.StringVar()
        self.status_var = tk.StringVar(value="Open an .mqblog file to begin.")

        self.info_vars = {
            "version": tk.StringVar(value="-"),
            "bitrate": tk.StringVar(value="-"),
            "size": tk.StringVar(value="-"),
            "records": tk.StringVar(value="-"),
            "frames": tk.StringVar(value="-"),
            "markers": tk.StringVar(value="-"),
            "unique_ids": tk.StringVar(value="-"),
            "duration": tk.StringVar(value="-"),
        }

        self._build_ui()

    def _build_ui(self) -> None:
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)

        main = ttk.Frame(self.root, padding=16)
        main.grid(row=0, column=0, sticky="nsew")
        main.columnconfigure(0, weight=1)

        title = ttk.Label(
            main,
            text="MQB Log Converter",
            font=("Segoe UI", 18, "bold"),
        )
        title.grid(row=0, column=0, sticky="w")

        subtitle = ttk.Label(
            main,
            text="Convert MQB Guided Logger .mqblog captures on your PC.",
        )
        subtitle.grid(row=1, column=0, sticky="w", pady=(0, 14))

        file_frame = ttk.LabelFrame(main, text="Source log", padding=12)
        file_frame.grid(row=2, column=0, sticky="ew")
        file_frame.columnconfigure(0, weight=1)

        path_entry = ttk.Entry(
            file_frame,
            textvariable=self.path_var,
            state="readonly",
        )
        path_entry.grid(row=0, column=0, sticky="ew", padx=(0, 8))

        ttk.Button(
            file_frame,
            text="Open .mqblog",
            command=self.open_file,
        ).grid(row=0, column=1)

        info_frame = ttk.LabelFrame(main, text="Log information", padding=12)
        info_frame.grid(row=3, column=0, sticky="ew", pady=(12, 0))
        info_frame.columnconfigure(1, weight=1)
        info_frame.columnconfigure(3, weight=1)

        rows = [
            ("Format version", "version", "Bitrate", "bitrate"),
            ("File size", "size", "Records", "records"),
            ("CAN frames", "frames", "Markers", "markers"),
            ("Unique CAN IDs", "unique_ids", "Duration", "duration"),
        ]

        for row_index, row in enumerate(rows):
            ttk.Label(info_frame, text=row[0]).grid(
                row=row_index,
                column=0,
                sticky="w",
                padx=(0, 8),
                pady=3,
            )

            ttk.Label(
                info_frame,
                textvariable=self.info_vars[row[1]],
                font=("Segoe UI", 9, "bold"),
            ).grid(
                row=row_index,
                column=1,
                sticky="w",
                padx=(0, 24),
                pady=3,
            )

            ttk.Label(info_frame, text=row[2]).grid(
                row=row_index,
                column=2,
                sticky="w",
                padx=(0, 8),
                pady=3,
            )

            ttk.Label(
                info_frame,
                textvariable=self.info_vars[row[3]],
                font=("Segoe UI", 9, "bold"),
            ).grid(
                row=row_index,
                column=3,
                sticky="w",
                pady=3,
            )

        export_frame = ttk.LabelFrame(main, text="Export", padding=12)
        export_frame.grid(row=4, column=0, sticky="ew", pady=(12, 0))

        ttk.Button(
            export_frame,
            text="Export CSV",
            command=lambda: self.export_file("csv"),
        ).grid(row=0, column=0, padx=(0, 8), pady=4)

        ttk.Button(
            export_frame,
            text="Export Vector ASC",
            command=lambda: self.export_file("asc"),
        ).grid(row=0, column=1, padx=(0, 8), pady=4)

        ttk.Button(
            export_frame,
            text="Export candump",
            command=lambda: self.export_file("candump"),
        ).grid(row=0, column=2, padx=(0, 8), pady=4)

        ttk.Button(
            export_frame,
            text="Export JSON",
            command=lambda: self.export_file("json"),
        ).grid(row=0, column=3, pady=4)

        ttk.Button(
            export_frame,
            text="Export all formats",
            command=self.export_all,
        ).grid(
            row=1,
            column=0,
            columnspan=4,
            sticky="ew",
            pady=(8, 0),
        )

        note_frame = ttk.LabelFrame(main, text="Format notes", padding=12)
        note_frame.grid(row=5, column=0, sticky="ew", pady=(12, 0))

        note = (
            "CSV includes timestamps, guided-test step numbers, markers, CAN IDs, "
            "DLC and data bytes.\n"
            "ASC is intended for Vector-compatible tools. candump is useful with "
            "Linux SocketCAN tools. JSON preserves structured records for scripts "
            "and further analysis."
        )

        ttk.Label(
            note_frame,
            text=note,
            wraplength=680,
            justify="left",
        ).grid(row=0, column=0, sticky="w")

        ttk.Separator(main).grid(
            row=6,
            column=0,
            sticky="ew",
            pady=(16, 8),
        )

        ttk.Label(
            main,
            textvariable=self.status_var,
        ).grid(row=7, column=0, sticky="w")

    def open_file(self) -> None:
        file_name = filedialog.askopenfilename(
            title="Open MQB log",
            filetypes=[
                ("MQB log files", "*.mqblog"),
                ("All files", "*.*"),
            ],
        )

        if not file_name:
            return

        path = Path(file_name)

        try:
            reader = MqbLogReader(path)
            header = reader.read_header()
            stats = reader.statistics()
        except (OSError, MqbLogError) as exc:
            messagebox.showerror(APP_NAME, str(exc))
            return

        self.current_path = path
        self.reader = reader
        self.path_var.set(str(path))

        self.info_vars["version"].set(str(header.format_version))
        self.info_vars["bitrate"].set(f"{header.bitrate:,} bit/s")
        self.info_vars["size"].set(format_bytes(stats.file_size))
        self.info_vars["records"].set(f"{stats.total_records:,}")
        self.info_vars["frames"].set(f"{stats.can_frames:,}")
        self.info_vars["markers"].set(f"{stats.markers:,}")
        self.info_vars["unique_ids"].set(f"{stats.unique_ids:,}")
        self.info_vars["duration"].set(
            format_duration(stats.duration_seconds)
        )

        self.status_var.set(
            f"Loaded {path.name}: "
            f"{stats.can_frames:,} CAN frames, "
            f"{stats.unique_ids:,} unique IDs."
        )

    def export_file(self, export_format: str) -> None:
        if self.current_path is None or self.reader is None:
            messagebox.showwarning(
                APP_NAME,
                "Open an .mqblog file first.",
            )
            return

        default_path = default_output_path(
            self.current_path,
            export_format,
        )

        file_types = {
            "csv": [("CSV files", "*.csv")],
            "asc": [("Vector ASC files", "*.asc")],
            "candump": [("candump log files", "*.log")],
            "json": [("JSON files", "*.json")],
        }

        output_name = filedialog.asksaveasfilename(
            title=f"Export {export_format.upper()}",
            initialdir=str(default_path.parent),
            initialfile=default_path.name,
            defaultextension=default_path.suffix,
            filetypes=file_types[export_format] + [("All files", "*.*")],
        )

        if not output_name:
            return

        output_path = Path(output_name)

        try:
            self.status_var.set(
                f"Exporting {output_path.name}..."
            )
            self.root.update_idletasks()

            EXPORTERS[export_format](
                self.reader,
                output_path,
            )
        except (OSError, MqbLogError) as exc:
            messagebox.showerror(APP_NAME, str(exc))
            self.status_var.set("Export failed.")
            return

        self.status_var.set(
            f"Export complete: {output_path}"
        )

        messagebox.showinfo(
            APP_NAME,
            f"Export complete.\n\n{output_path}",
        )

    def export_all(self) -> None:
        if self.current_path is None or self.reader is None:
            messagebox.showwarning(
                APP_NAME,
                "Open an .mqblog file first.",
            )
            return

        output_folder = filedialog.askdirectory(
            title="Select export folder",
            initialdir=str(self.current_path.parent),
        )

        if not output_folder:
            return

        folder = Path(output_folder)
        created: list[Path] = []

        try:
            for export_format, exporter in EXPORTERS.items():
                suffix = {
                    "csv": ".csv",
                    "asc": ".asc",
                    "candump": ".log",
                    "json": ".json",
                }[export_format]

                output_path = folder / (
                    self.current_path.stem + suffix
                )

                self.status_var.set(
                    f"Exporting {output_path.name}..."
                )
                self.root.update_idletasks()

                exporter(self.reader, output_path)
                created.append(output_path)

        except (OSError, MqbLogError) as exc:
            messagebox.showerror(APP_NAME, str(exc))
            self.status_var.set("Export failed.")
            return

        self.status_var.set(
            f"Exported {len(created)} files to {folder}"
        )

        messagebox.showinfo(
            APP_NAME,
            f"Exported {len(created)} formats to:\n\n{folder}",
        )


def run_cli(args: argparse.Namespace) -> int:
    input_path = Path(args.input)

    try:
        reader = MqbLogReader(input_path)
        header = reader.read_header()
        stats = reader.statistics()
    except (OSError, MqbLogError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    if args.info:
        print(f"File:       {input_path}")
        print(f"Version:    {header.format_version}")
        print(f"Bitrate:    {header.bitrate}")
        print(f"Size:       {stats.file_size} bytes")
        print(f"Records:    {stats.total_records}")
        print(f"CAN frames: {stats.can_frames}")
        print(f"Markers:    {stats.markers}")
        print(f"Unique IDs: {stats.unique_ids}")
        print(f"Duration:   {stats.duration_seconds:.6f} s")

    formats = args.format or []

    for export_format in formats:
        output_path = (
            Path(args.output)
            if args.output and len(formats) == 1
            else default_output_path(input_path, export_format)
        )

        try:
            EXPORTERS[export_format](reader, output_path)
        except (OSError, MqbLogError) as exc:
            print(
                f"Error exporting {export_format}: {exc}",
                file=sys.stderr,
            )
            return 1

        print(f"Exported {export_format}: {output_path}")

    return 0


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert MQB Guided Logger .mqblog files."
    )

    parser.add_argument(
        "input",
        nargs="?",
        help="Input .mqblog file. If omitted, the GUI is started.",
    )

    parser.add_argument(
        "-f",
        "--format",
        action="append",
        choices=sorted(EXPORTERS.keys()),
        help="Export format. Can be specified more than once.",
    )

    parser.add_argument(
        "-o",
        "--output",
        help="Output path. Only used when exporting one format.",
    )

    parser.add_argument(
        "--info",
        action="store_true",
        help="Print log information.",
    )

    return parser.parse_args()


def main() -> int:
    args = parse_arguments()

    if args.input:
        if not args.info and not args.format:
            args.info = True

        return run_cli(args)

    root = tk.Tk()

    try:
        style = ttk.Style(root)

        if "vista" in style.theme_names():
            style.theme_use("vista")
    except tk.TclError:
        pass

    ConverterGui(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
