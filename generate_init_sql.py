import json
import sys
import os
from datetime import datetime


def extract_rsrp(cell):
    """RSRP"""
    cell_type = cell.get("type", "UNKNOWN")

    if cell_type == "LTE":
        return cell.get("signal", {}).get("rsrp", -145.0)
    elif cell_type == "NR":
        return cell.get("signal", {}).get("ssRsrp", -145.0)
    elif cell_type == "GSM":
        return cell.get("signal", {}).get("dbm", -145.0)
    return -145.0


def extract_rsrq(cell):
    """RSRQ"""
    cell_type = cell.get("type", "UNKNOWN")

    if cell_type == "LTE":
        return cell.get("signal", {}).get("rsrq", -50.0)
    elif cell_type == "NR":
        return cell.get("signal", {}).get("ssRsrq", -50.0)
    elif cell_type == "GSM":
        return cell.get("signal", {}).get("rssi", -120.0)
    return -50.0


def extract_rssi(cell):
    """RSSI"""
    signal = cell.get("signal", {})

    if "rssi" in signal:
        return signal["rssi"]
    if "dbm" in signal:
        return signal["dbm"]

    return -120.0


def process_json_file(json_filepath, output_sql_filepath):
    records = []

    with open(json_filepath, 'r') as f:
        for line_num, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue

            try:
                data = json.loads(line)

                lat = data.get("lat", 0.0)
                lon = data.get("lon", 0.0)
                alt = data.get("alt", 0.0)
                acc = data.get("acc", 0.0)

                cells = data.get("cell_data", {}).get("cells", [])
                registered_cell = None

                for cell in cells:
                    if cell.get("registered", False):
                        registered_cell = cell
                        break

                if registered_cell:
                    network_type = registered_cell.get("type", "UNKNOWN")
                    rsrp = extract_rsrp(registered_cell)
                    rsrq = extract_rsrq(registered_cell)
                    rssi = extract_rssi(registered_cell)
                else:
                    network_type = cells[0].get("type", "UNKNOWN") if cells else "UNKNOWN"

                    if cells:
                        rsrp = extract_rsrp(cells[0])
                        rsrq = extract_rsrq(cells[0])
                        rssi = extract_rssi(cells[0])
                    else:
                        rsrp = -145.0
                        rsrq = -50.0
                        rssi = -120.0

                records.append({
                    'lat': lat,
                    'lon': lon,
                    'alt': alt,
                    'acc': acc,
                    'network_type': network_type,
                    'rsrp': rsrp,
                    'rsrq': rsrq,
                    'rssi': rssi
                })

            except json.JSONDecodeError as e:
                print(f"JSON error line {line_num}: {e}", file=sys.stderr)
            except Exception as e:
                print(f"Processing error line {line_num}: {e}", file=sys.stderr)

    with open(output_sql_filepath, 'w') as f:
        f.write("-- Auto generated network_logs DB init\n\n")

        f.write("CREATE TABLE IF NOT EXISTS network_logs (\n")
        f.write("    id SERIAL PRIMARY KEY,\n")
        f.write("    latitude DOUBLE PRECISION NOT NULL,\n")
        f.write("    longitude DOUBLE PRECISION NOT NULL,\n")
        f.write("    altitude DOUBLE PRECISION,\n")
        f.write("    accuracy DOUBLE PRECISION,\n")
        f.write("    network_type VARCHAR(10),\n")
        f.write("    rsrp DOUBLE PRECISION,\n")
        f.write("    rsrq DOUBLE PRECISION,\n")
        f.write("    rssi DOUBLE PRECISION,\n")
        f.write("    timestamp TIMESTAMPTZ DEFAULT NOW()\n")
        f.write(");\n\n")

        f.write("INSERT INTO network_logs "
                "(latitude, longitude, altitude, accuracy, network_type, rsrp, rsrq, rssi)\n")
        f.write("VALUES\n")

        for i, record in enumerate(records):
            comma = "," if i < len(records) - 1 else ";"
            f.write(
                f"    ({record['lat']}, {record['lon']}, {record['alt']}, "
                f"{record['acc']}, "
                f"'{record['network_type']}', "
                f"{record['rsrp']}, "
                f"{record['rsrq']}, "
                f"{record['rssi']}){comma}\n"
            )

        f.write(f"\n-- Total records: {len(records)}\n")

    print(f"SQL file created: {output_sql_filepath}")
    print(f"Records processed: {len(records)}")

    return records


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 generate_init_sql.py <json_file> [output_sql]")
        sys.exit(1)

    json_file = sys.argv[1]

    if len(sys.argv) >= 3:
        output_file = sys.argv[2]
    else:
        os.makedirs("db/init", exist_ok=True)
        output_file = "db/init/1-init.sql"

    if not os.path.exists(json_file):
        print(f"File not found: {json_file}", file=sys.stderr)
        sys.exit(1)

    records = process_json_file(json_file, output_file)

    stats = {}
    for r in records:
        stats[r['network_type']] = stats.get(r['network_type'], 0) + 1

    print("\nNetwork types stats:")
    for k, v in sorted(stats.items(), key=lambda x: x[1], reverse=True):
        print(f"  {k}: {v}")


if __name__ == "__main__":
    main()