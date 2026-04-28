"""OpenEMS Dashboard launcher.

Usage: python run_dashboard.py [--port PORT] [--shm-name NAME]
"""

import os
import sys

import uvicorn

def main():
    port = 8080
    shm_name = os.getenv("OPENEMS_SHM_NAME") or ("Local\\openems_rt_db" if os.name == "nt" else "/openems_rt_db")

    i = 1
    while i < len(sys.argv):
        if sys.argv[i] == "--port" and i + 1 < len(sys.argv):
            port = int(sys.argv[i + 1])
            i += 2
        elif sys.argv[i] == "--shm-name" and i + 1 < len(sys.argv):
            shm_name = sys.argv[i + 1]
            os.environ["OPENEMS_SHM_NAME"] = shm_name
            i += 2
        else:
            i += 1

    print(f"OpenEMS Dashboard starting on http://localhost:{port}")
    print(f"Shared memory: {shm_name}")
    uvicorn.run("admin_server:app", host="0.0.0.0", port=port, log_level="info")

if __name__ == "__main__":
    main()
