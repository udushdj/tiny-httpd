#!/usr/bin/env python3
import os, sys

print("Content-Type: text/plain; charset=utf-8\r\n\r\n")

content_len = int(os.environ.get("CONTENT_LENGTH", "0"))
if content_len > 0:
    body = sys.stdin.read(content_len)
    print(f"POST body ({content_len} bytes):\n{body}")
else:
    print("No POST body received.")
