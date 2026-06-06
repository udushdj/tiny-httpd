#!/usr/bin/env python3
"""echo.py - CGI POST body 回显脚本"""
import sys
import os
import cgi
import cgitb
cgitb.enable()

def main():
    method = os.environ.get("REQUEST_METHOD", "unknown")
    content_length = int(os.environ.get("CONTENT_LENGTH", 0))

    print("Content-Type: text/html; charset=utf-8")
    print()

    print("<html><head><title>CGI POST Echo</title></head><body>")
    print("<h1>POST Body Echo</h1>")
    print(f"<p>Method: {method}</p>")
    print(f"<p>Content-Length: {content_length}</p>")

    if method == "POST" and content_length > 0:
        body = sys.stdin.read(content_length)
        print(f"<h2>Body:</h2><pre>{cgi.escape(body)}</pre>")

        # 解析表单参数
        if "application/x-www-form-urlencoded" in os.environ.get("CONTENT_TYPE", ""):
            parsed = cgi.parse_qs(body)
            if parsed:
                print("<h2>Parsed Parameters:</h2><ul>")
                for key, values in parsed.items():
                    for v in values:
                        print(f"<li>{cgi.escape(key)} = {cgi.escape(v)}</li>")
                print("</ul>")
    else:
        print("<p>No POST data received.</p>")
        print(f"<p>Query: {os.environ.get('QUERY_STRING', '(none)')}</p>")

    print("</body></html>")

if __name__ == "__main__":
    main()
