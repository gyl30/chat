#ifndef CHAT_QT_SRC_CLIENT_BRIDGE_HPP
#define CHAT_QT_SRC_CLIENT_BRIDGE_HPP

#include <memory>
#include <optional>

#include <QObject>
#include <QString>
#include <QtGlobal>

#include "conversation_data.hpp"
#include "message_data.hpp"

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
    void get_messages(qint64 user, std::optional<qint64> before = {});
    void send_message(qint64 user, QString text);
    void mark_read(qint64 user, qint64 message);

   signals:
    void connected();
    void disconnected();
    void error(QString message);
    void authentication_finished(bool authenticated, QString error_message);
    void conversations_received(QList<conversation_data> conversations, QString error_message);
    void messages_received(qint64 user, QList<message_data> messages, bool older, QString error_message);
    void message_received(message_data message);
    void message_sent(qint64 user, QString text, qint64 message, bool realtime, QString error_message);
    void read_marked(qint64 user, qint64 message, QString error_message);

   private:
    std::unique_ptr<chat::client> client_;
};

#endif
