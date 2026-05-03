import json
import sys
import os
from datetime import datetime

def extract_rsrp(cell):
    """Извлекает RSRP в зависимости от типа сети"""
    cell_type = cell.get("type", "UNKNOWN")
    
    if cell_type == "LTE":
        return cell.get("signal", {}).get("rsrp", -145.0)
    elif cell_type == "NR":
        return cell.get("signal", {}).get("ssRsrp", -145.0)
    elif cell_type == "GSM":
        return cell.get("signal", {}).get("dbm", -145.0)
    else:
        return -145.0

def process_json_file(json_filepath, output_sql_filepath):
    """Обрабатывает JSON файл и создает SQL скрипт"""
    
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
                else:

                    network_type = cells[0].get("type", "UNKNOWN") if cells else "UNKNOWN"
                    rsrp = extract_rsrp(cells[0]) if cells else -145.0
                
                records.append({
                    'lat': lat,
                    'lon': lon,
                    'alt': alt,
                    'acc': acc,
                    'network_type': network_type,
                    'rsrp': rsrp
                })
                
            except json.JSONDecodeError as e:
                print(f"Ошибка парсинга JSON на строке {line_num}: {e}", file=sys.stderr)
                continue
            except Exception as e:
                print(f"Ошибка обработки строки {line_num}: {e}", file=sys.stderr)
                continue

    with open(output_sql_filepath, 'w') as f:
        f.write("-- Инициализация базы данных network_logs\n")
        f.write("-- Сгенерировано автоматически\n\n")

        f.write("CREATE TABLE IF NOT EXISTS network_logs (\n")
        f.write("    id SERIAL PRIMARY KEY,\n")
        f.write("    latitude DOUBLE PRECISION NOT NULL,\n")
        f.write("    longitude DOUBLE PRECISION NOT NULL,\n")
        f.write("    altitude DOUBLE PRECISION,\n")
        f.write("    accuracy DOUBLE PRECISION,\n")
        f.write("    network_type VARCHAR(10),\n")
        f.write("    rsrp DOUBLE PRECISION,\n")
        f.write("    timestamp TIMESTAMPTZ DEFAULT NOW()\n")
        f.write(");\n\n")

        f.write("-- Вставка исторических данных\n")
        f.write("INSERT INTO network_logs (latitude, longitude, altitude, accuracy, network_type, rsrp) VALUES\n")
        
        for i, record in enumerate(records):
            comma = "," if i < len(records) - 1 else ";"
            f.write(f"    ({record['lat']}, {record['lon']}, {record['alt']}, "
                   f"{record['acc']}, '{record['network_type']}', {record['rsrp']}){comma}\n")
        
        f.write(f"\n-- Всего записей: {len(records)}\n")
    
    print(f"SQL файл создан: {output_sql_filepath}")
    print(f"Всего обработано записей: {len(records)}")
    
    return records

def main():
    if len(sys.argv) < 2:
        print("Использование: python3 generate_init_sql.py <json_file> [output_sql_file]")
        print("Пример: python3 generate_init_sql.py log.json")
        print("        python3 generate_init_sql.py log.json db/init/1-init.sql")
        sys.exit(1)
    
    json_file = sys.argv[1]
    
    if len(sys.argv) >= 3:
        output_file = sys.argv[2]
    else:
        os.makedirs("db/init", exist_ok=True)
        output_file = "db/init/1-init.sql"
    
    if not os.path.exists(json_file):
        print(f"Ошибка: файл {json_file} не найден", file=sys.stderr)
        sys.exit(1)
    
    records = process_json_file(json_file, output_file)

    types = {}
    for r in records:
        types[r['network_type']] = types.get(r['network_type'], 0) + 1
    
    print("\nСтатистика по типам сетей:")
    for net_type, count in sorted(types.items(), key=lambda x: x[1], reverse=True):
        print(f"  {net_type}: {count}")

if __name__ == "__main__":
    main()