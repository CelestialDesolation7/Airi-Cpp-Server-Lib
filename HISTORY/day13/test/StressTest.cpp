#include "Buffer.h"
#include "InetAddress.h"
#include "Socket.h"
#include "util.h"
#include <functional>
#include <iostream>
#include <mutex>
#include <string.h>
#include <string>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std;

int failed_count = 0;
mutex failed_mutex;

void oneClient(int id, int msgs) {
    Socket *sock = new Socket();
    InetAddress *addr = new InetAddress("127.0.0.1", 8888);

    // 连服务器
    if (connect(sock->getFd(), (sockaddr *)&addr->addr, addr->addr_len) == -1) {
        cout << "Client " << id << " connect error" << endl;
        return;
    }

    Buffer *readBuffer = new Buffer();
    char buf[1024];

    for (int i = 0; i < msgs; ++i) {
        string msg = "Client " + to_string(id) + " msg " + to_string(i);
        // 发数据
        ssize_t write_bytes = write(sock->getFd(), msg.c_str(), msg.size());
        if (write_bytes == -1) {
            cout << "Client " << id << " write error" << endl;
            break;
        }

        int already_read = 0;
        int target_len = msg.size();

        while (already_read < target_len) {
            bzero(&buf, sizeof(buf));
            ssize_t read_bytes = read(sock->getFd(), buf, sizeof(buf));
            if (read_bytes > 0) {
                readBuffer->append(buf, read_bytes);
                already_read += read_bytes;
            } else if (read_bytes == 0) {
                cout << "client " << id << " disconnected" << endl;
                goto end;
            }
        }

        if (readBuffer->retrieveAsString(target_len) != msg) {
            lock_guard<mutex> lock(failed_mutex);
            failed_count++;
            cout << "client " << id << " msg " << i << " verify failed!" << endl;
        }
    }
end:
    delete readBuffer;
    delete sock;
    delete addr;
}

int main(int argc, char *argv[]) {
    int threads_num = 100;
    int msgs_num = 10000;
    int wait_seconds = 0;

    if (argc > 1)
        threads_num = atoi(argv[1]);
    if (argc > 2)
        msgs_num = atoi(argv[2]);
    if (argc > 3)
        wait_seconds = atoi(argv[3]);

    vector<thread> threads;
    for (int i = 0; i < threads_num; ++i) {
        threads.emplace_back(oneClient, i, msgs_num);
        if (wait_seconds > 0) {
            sleep(wait_seconds); // 给的等待时间越小压力越大
        }
    }

    for (auto &t : threads) {
        if (t.joinable())
            t.join();
    }
    cout << "[result] Test Finished. Failed count: " << failed_count << endl;
    return 0;
}