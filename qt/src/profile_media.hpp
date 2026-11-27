#ifndef CHAT_QT_SRC_PROFILE_MEDIA_HPP
#define CHAT_QT_SRC_PROFILE_MEDIA_HPP

#include <QTimer>
#include <QWidget>
#include <array>
#include <optional>

class chat_widget;
class client_bridge;
struct message_data;
class QPushButton;

class profile_media final : public QWidget
{
    Q_OBJECT

   public:
    profile_media(client_bridge& client, chat_widget& chat, QWidget* parent);
    void set_conversation(qint64 conversation);

   signals:
    void history_requested(int category);

   private:
    struct category_entry
    {
        QPushButton* button = nullptr;
        // No value before confirmation. Zero is an authoritative empty result.
        std::optional<qint64> head;
        quint64 revision = 0;
        bool dirty = false;
        bool requesting = false;
    };

    void schedule(category_entry& entry);
    void request();
    void reset();
    void update_entries();
    void on_message(message_data const& message, bool updated);

    client_bridge& client_;
    chat_widget& chat_;
    qint64 conversation_ = 0;
    std::array<category_entry, 3> entries_;
    QTimer refresh_;
};

#endif
