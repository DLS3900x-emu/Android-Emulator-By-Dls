import http.server, socketserver, threading, socket, os, sys
os.chdir(os.path.dirname(os.path.abspath(__file__)))
class Q(http.server.SimpleHTTPRequestHandler):
    def log_message(self,*a): pass
socketserver.TCPServer.allow_reuse_address=True
h=socketserver.TCPServer(('127.0.0.1',18080),Q)
threading.Thread(target=h.serve_forever,daemon=True).start()
u=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); u.bind(('127.0.0.1',18081))
while True:
    d,a=u.recvfrom(2000); u.sendto(b'echo:'+d,a)
