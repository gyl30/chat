#ifndef CHAT_QT_SRC_ATTACHMENT_DIALOG_HPP
#define CHAT_QT_SRC_ATTACHMENT_DIALOG_HPP

#include <QByteArray>
#include <QDialog>

class QLabel;
class QPushButton;

class attachment_dialog final : public QDialog
{
    Q_OBJECT

   public:
    attachment_dialog(qint64 conversation, qint64 message, QString filename, bool preview, QWidget* parent);
    void set_data(qint64 conversation, qint64 message, QByteArray data, QString const& error_message);

   private:
    qint64 conversation_;
    qint64 message_;
    QString filename_;
    bool preview_;
    QByteArray data_;
    QLabel* status_;
    QLabel* image_;
    QPushButton* save_button_;
};

#endif
