#!/usr/bin/env python3
import os, time, urllib.parse

print("Content-Type: text/html; charset=utf-8\r\n\r\n")

method = os.environ.get("REQUEST_METHOD", "GET")
query  = os.environ.get("QUERY_STRING", "")
params = urllib.parse.parse_qs(query)

print("<html><head><title>CGI Test</title></head><body>")
print("<h1>Hello from CGI!</h1>")
print(f"<p>Method: {method}</p>")
print(f"<p>Time: {time.ctime()}</p>")
print("<h2>Parameters:</h2><ul>")
for k, v in params.items():
    print(f"<li>{k} = {v[0]}</li>")
print("</ul></body></html>")
