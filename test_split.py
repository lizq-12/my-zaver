import socket
import time

def test_split_request(port):
    print(f"Testing Port {port}...", end=" ")
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5)
        # 这里的 127.0.0.1 指的是虚拟机自己，因为脚本和服务器在同一台机器上
        s.connect(('127.0.0.1', port))
        
        # 模拟 TCP 拆包：一个完整的请求被切成小碎片发送
        # 这能检测你的状态机是否还在“傻等”一个完整的包
        parts = [
            b"GET /index.html HT",       # 第一片
            b"TP/1.1\r\nHo",             # 第二片
            b"st: 127.0.0.1\r\n",        # ...
            b"Connection: keep-alive\r\n\r\n"
        ]
        
        for part in parts:
            s.send(part)
            # 关键：每发一点睡 0.1 秒，强制让 epoll 触发多次
            # 如果你的服务器逻辑不对，可能收到第一片就报错，或者一直卡住
            time.sleep(0.1) 
            
        response = s.recv(4096)
        # 解码并打印一部分响应来看看
        print(f"Response: {response[:20]}...")
        
        if b"200 OK" in response:
            print("PASS (Successfully handled split packets)")
        else:
            print("FAIL (Invalid response)")
        s.close()
    except Exception as e:
        print(f"FAIL (Connection Error: {e})")

if __name__ == "__main__":
    # 请确保这里的 8080 是你 zaver 配置文件里的端口
    test_split_request(3000)
