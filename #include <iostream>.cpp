#include <iostream>
#include <boost/asio.hpp>
#include <boost/bind/bind.hpp>
#include <fstream>
#include <sstream>
#include <ctime>
#include <unordered_map>
#include <mutex>
#include <vector>

using boost::asio::ip::tcp;

// Estrutura para armazenar os registros de log
#pragma pack(push, 1)
struct LogRecord {
    char sensor_id[32];
    std::time_t timestamp;
    double value;
};
#pragma pack(pop)

std::unordered_map<std::string, std::mutex> file_mutexes;
std::mutex global_mutex;

// Funções auxiliares para conversão de timestamp e strings
std::time_t string_to_time_t(const std::string& time_string) {
    std::tm tm = {};
    std::istringstream ss(time_string);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return std::mktime(&tm);
}

std::string time_t_to_string(std::time_t time) {
    std::tm* tm = std::localtime(&time);
    std::ostringstream ss;
    ss << std::put_time(tm, "%Y-%m-%dT%H:%M:%S");
    return ss.str();
}

// Classe de conexão
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket) : socket_(std::move(socket)) {}

    void start() {
        do_read();
    }

private:
    tcp::socket socket_;
    enum { max_length = 1024 };
    char data_[max_length];

    void do_read() {
        auto self(shared_from_this());
        socket_.async_read_some(
            boost::asio::buffer(data_, max_length),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    process_message(std::string(data_, length));
                    do_read();
                }
            });
    }

    void process_message(const std::string& message) {
        if (message.starts_with("LOG|")) {
            handle_log_message(message);
        } else if (message.starts_with("GET|")) {
            handle_get_message(message);
        } else {
            send_response("ERROR|INVALID_COMMAND\r\n");
        }
    }

    void handle_log_message(const std::string& message) {
        std::istringstream ss(message);
        std::string command, sensor_id, timestamp_str, value_str;
        std::getline(ss, command, '|');
        std::getline(ss, sensor_id, '|');
        std::getline(ss, timestamp_str, '|');
        std::getline(ss, value_str, '\r');

        std::time_t timestamp = string_to_time_t(timestamp_str);
        double value = std::stod(value_str);

        LogRecord record;
        std::strncpy(record.sensor_id, sensor_id.c_str(), sizeof(record.sensor_id) - 1);
        record.sensor_id[sizeof(record.sensor_id) - 1] = '\0';
        record.timestamp = timestamp;
        record.value = value;

        std::lock_guard<std::mutex> lock(file_mutexes[sensor_id]);
        std::ofstream ofs(sensor_id + ".log", std::ios::binary | std::ios::app);
        ofs.write(reinterpret_cast<const char*>(&record), sizeof(record));
    }

    void handle_get_message(const std::string& message) {
        std::istringstream ss(message);
        std::string command, sensor_id, num_records_str;
        std::getline(ss, command, '|');
        std::getline(ss, sensor_id, '|');
        std::getline(ss, num_records_str, '\r');

        int num_records = std::stoi(num_records_str);

        std::vector<LogRecord> records;
        {
            std::lock_guard<std::mutex> lock(file_mutexes[sensor_id]);
            std::ifstream ifs(sensor_id + ".log", std::ios::binary);

            if (!ifs) {
                send_response("ERROR|INVALID_SENSOR_ID\r\n");
                return;
            }

            LogRecord record;
            while (ifs.read(reinterpret_cast<char*>(&record), sizeof(record))) {
                records.push_back(record);
            }
        }

        int total_records = records.size();
        int start_index = std::max(0, total_records - num_records);
        std::ostringstream response;
        response << total_records;

        for (int i = start_index; i < total_records; ++i) {
            response << ";" << time_t_to_string(records[i].timestamp) << "|" << records[i].value;
        }

        response << "\r\n";
        send_response(response.str());
    }

    void send_response(const std::string& response) {
        auto self(shared_from_this());
        boost::asio::async_write(
            socket_, boost::asio::buffer(response),
            [this, self](boost::system::error_code ec, std::size_t /*length*/) {
                if (ec) {
                    std::cerr << "Failed to send response: " << ec.message() << std::endl;
                }
            });
    }
};

// Classe do servidor
class Server {
public:
    Server(boost::asio::io_context& io_context, short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
        do_accept();
    }

private:
    tcp::acceptor acceptor_;

    void do_accept() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<Session>(std::move(socket))->start();
                }
                do_accept();
            });
    }
};

int main(int argc, char* argv[]) {
    try {
        if (argc != 2) {
            std::cerr << "Usage: server <port>\n";
            return 1;
        }

        boost::asio::io_context io_context;
        Server server(io_context, std::atoi(argv[1]));
        io_context.run();
    } catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    return 0;
}
