import time
import zmq
import os
from datetime import datetime

LOG_FILE = "server_received_data.txt"

def save_to_file(count, data):
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    log_entry = f"[{timestamp}] Packet #{count}: {data}\n"
    with open(LOG_FILE, "a", encoding="utf-8") as f:
        f.write(log_entry)

def print_all_data():
    print(f"CONTENTS OF {LOG_FILE}:")
    if os.path.exists(LOG_FILE):
        with open(LOG_FILE, "r", encoding="utf-8") as f:
            content = f.read()
            if content:
                print(content)
            else:
                print("[File is empty]")
    else:
        print("[File does not exist]")

context = zmq.Context()
socket = context.socket(zmq.REP)
socket.bind("tcp://0.0.0.0:7777") 
    
print(f"Server started on port 7777. Saving data to '{LOG_FILE}'...")
print("Press Ctrl+C to stop the server and view logs.")

packet_count = 0

try:
    while True:
        message_bytes = socket.recv()
        packet_count += 1
        message_str = message_bytes.decode('utf-8')
        print(f"[SERVER] Received packet #{packet_count}: {message_str}")
        save_to_file(packet_count, message_str)
        if message_str == "PRINT_LOG":
            print_all_data()
        time.sleep(0.5) 
        response_text = f"Server received packet #{packet_count}. Data from server \"Hello from VPS!\""
        socket.send(response_text.encode('utf-8'))

except KeyboardInterrupt:
    print("\n\n[STOP COMMAND DETECTED]")
    print_all_data()
        
except Exception as e:
    print(f"SERVER ERROR: {e}")
        
finally:
    socket.close()
    context.term()
    print("Server stopped")
