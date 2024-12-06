#include <boost/asio.hpp>
#include <iostream>
#include <sstream>
#include <ctime>
#include <vector>
#include <unordered_map>
#include <string>
#include <iomanip>
#include <fstream>

using boost::asio::ip::tcp;
namespace asio = boost::asio;

// Definição do registro no arquivo binário (formato: sensor_id, timestamp, value)
#pragma pack(push, 1)
struct LogRecord {
    char sensor_id[32];   // ID do sensor (até 32 caracteres)
    std::time_t timestamp; // Timestamp Unix (em segundos desde a época Unix)
    double value;          // Valor da leitura
};
#pragma pack(pop)

class SensorServer {
public:
    SensorServer(asio::io_context& io_context, short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
        start_accept();
    }

private:
    void start_accept() {
        tcp::socket socket(acceptor_.get_executor());
        acceptor_.async_accept(socket, [this, &socket](boost::system::error_code ec) {
            if (!ec) {
                std::make_shared<Session>(std::move(socket), sensors_)->start();
            }
            start_accept();
        });
    }

    // A classe Session representa uma conexão com um cliente
    class Session : public std::enable_shared_from_this<Session> {
    public:
        Session(tcp::socket socket, std::unordered_map<std::string, std::vector<LogRecord>>& sensors)
            : socket_(std::move(socket)), sensors_(sensors) {}

        void start() {
            do_read();
        }

    private:
        void do_read() {
            auto self(shared_from_this());
            socket_.async_read_some(asio::buffer(data_, max_length),
                [this, self](boost::system::error_code ec, std::size_t length) {
                    if (!ec) {
                        process_request(length);
                        do_read(); // Continuar lendo
                    }
                });
        }

        // Função para processar a solicitação do cliente
        void process_request(std::size_t length) {
            std::string request(data_, length);
            std::cout << "Request: " << request << std::endl;

            if (request.substr(0, 3) == "LOG") {
                // Processar dados de sensor (LOG)
                process_log_request(request);
            } else if (request.substr(0, 3) == "GET") {
                // Processar solicitação de registros (GET)
                process_get_request(request);
            } else {
                // Mensagem de erro
                std::string response = "ERROR|INVALID_REQUEST\r\n";
                do_write(response);
            }
        }

        // Processa um request de tipo LOG (dados de sensor)
        void process_log_request(const std::string& request) {
            // Exemplo de formato: LOG|SENSOR_ID|2023-05-11T15:30:00|78.5
            std::string sensor_id = request.substr(4, request.find('|', 4) - 4);
            std::string timestamp_str = request.substr(request.find('|', 4) + 1, 19);
            double value = std::stod(request.substr(request.find_last_of('|') + 1));

            std::time_t timestamp = string_to_time_t(timestamp_str);

            // Criar o registro e salvar no arquivo binário
            LogRecord log_record;
            std::strncpy(log_record.sensor_id, sensor_id.c_str(), sizeof(log_record.sensor_id));
            log_record.timestamp = timestamp;
            log_record.value = value;

            // Gravar o log no arquivo binário
            save_log_to_file(log_record);

            // Responder com sucesso
            std::string response = "LOG|RECEIVED\r\n";
            do_write(response);
        }

        // Processa a solicitação de registros (GET)
        void process_get_request(const std::string& request) {
            // Exemplo de formato: GET|SENSOR_ID|10
            std::string sensor_id = request.substr(4, request.find('|', 4) - 4);
            int num_records = std::stoi(request.substr(request.find_last_of('|') + 1));

            // Buscar registros do sensor no arquivo binário
            std::vector<LogRecord> records = get_sensor_records(sensor_id, num_records);

            // Formatar resposta
            std::ostringstream oss;
            oss << records.size();
            for (const auto& record : records) {
                oss << ";" << time_t_to_string(record.timestamp) << "|" << record.value;
            }
            oss << "\r\n";

            // Enviar a resposta
            do_write(oss.str());
        }

        // Função para salvar um log em um arquivo binário
        void save_log_to_file(const LogRecord& log_record) {
            std::ofstream file("sensor_logs.bin", std::ios::binary | std::ios::app);
            if (file.is_open()) {
                file.write(reinterpret_cast<const char*>(&log_record), sizeof(log_record));
                file.close();
            } else {
                std::cerr << "Erro ao abrir o arquivo de logs" << std::endl;
            }
        }

        // Função para buscar registros de um sensor no arquivo binário
        std::vector<LogRecord> get_sensor_records(const std::string& sensor_id, int num_records) {
            std::vector<LogRecord> records;
            std::ifstream file("sensor_logs.bin", std::ios::binary);
            if (file.is_open()) {
                LogRecord record;
                while (file.read(reinterpret_cast<char*>(&record), sizeof(record))) {
                    if (sensor_id == record.sensor_id) {
                        records.push_back(record);
                    }
                }
                file.close();
            } else {
                std::cerr << "Erro ao abrir o arquivo de logs" << std::endl;
            }

            // Limitar o número de registros solicitados
            if (records.size() > num_records) {
                records.resize(num_records);
            }

            return records;
        }

        // Função para enviar uma resposta para o cliente
        void do_write(const std::string& response) {
            auto self(shared_from_this());
            asio::async_write(socket_, asio::buffer(response),
                [this, self](boost::system::error_code ec, std::size_t /*length*/) {
                    if (ec) {
                        std::cerr << "Erro ao enviar resposta: " << ec.message() << std::endl;
                    }
                });
        }

        // Função para converter string de data/hora para std::time_t
        std::time_t string_to_time_t(const std::string& time_string) {
            std::tm tm = {};
            std::istringstream ss(time_string);
            ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
            return std::mktime(&tm); // Converte para timestamp Unix
        }

        // Função para converter std::time_t para string de data/hora
        std::string time_t_to_string(std::time_t time) {
            std::tm* tm = std::localtime(&time);
            std::ostringstream ss;
            ss << std::put_time(tm, "%Y-%m-%dT%H:%M:%S");
            return ss.str(); // Converte de volta para string formatada
        }

        tcp::socket socket_;
        static constexpr int max_length = 1024;
        char data_[max_length];
        std::unordered_map<std::string, std::vector<LogRecord>>& sensors_;
    };

    tcp::acceptor acceptor_;
    std::unordered_map<std::string, std::vector<LogRecord>> sensors_;
};

int main() {
    try {
        asio::io_context io_context;
        SensorServer server(io_context, 9000);
        io_context.run();
    } catch (const std::exception& e) {
        std::cerr << "Erro: " << e.what() << std::endl;
    }
    return 0;
}
