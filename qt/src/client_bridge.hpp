#ifndef CHAT_QT_SRC_CLIENT_BRIDGE_HPP
#define CHAT_QT_SRC_CLIENT_BRIDGE_HPP

#include <memory>

#include <QObject>
#include <QString>

#include "conversation_data.hpp"

namespace chat
{
class client;
}

class client_bridge final : public QObject
{
    Q_OBJECT

   public:
    explicit client_bridge(QObject* parent = nullptr);
    ~client_bridge() override;

    void connect_to_server(QString const& url);
    void close();
    void authenticate(QString const& username, QString const& password);
    void get_conversations();

   signals:
    void connected();
    void disconnected();
    void error(QString message);
    void authentication_finished(bool authenticated, QString error_message);
    void conversations_received(QList<conversation_data> conversations, QString error_message);

   private:
    std::unique_ptr<chat::client> client_;
};

#endif
