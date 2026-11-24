#include "bundle_dialog.hpp"
#include "attachment_dialog.hpp"
#include "chat_widget.hpp"
#include "client_bridge.hpp"
#include "message_markdown.hpp"
#include <QDateTime>
#include <QDialog>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QDesktopServices>
#include "message_links.hpp"

void show_bundle(client_bridge& client, chat_widget& chat, qint64 room, qint64 message, QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("聊天记录"));
    dialog.resize(560, 540);
    auto* layout = new QVBoxLayout(&dialog);
    auto* entries = new QListWidget(&dialog);
    auto* body = new QTextBrowser(&dialog);
    body->setOpenLinks(false);
    QObject::connect(body,
                     &QTextBrowser::anchorClicked,
                     &dialog,
                     [](QUrl const& url)
                     {
                         auto allowed = http_message_url(url.toString(QUrl::FullyEncoded));
                         if (!allowed.isEmpty())
                         {
                             QDesktopServices::openUrl(allowed);
                         }
                     });
    auto* file = new QPushButton(QStringLiteral("查看附件"), &dialog);
    file->hide();
    auto* more = new QPushButton(QStringLiteral("加载更多"), &dialog);
    more->setEnabled(false);
    layout->addWidget(entries);
    layout->addWidget(body);
    layout->addWidget(file);
    layout->addWidget(more);
    std::vector<chat::forwarded_item> items;
    qint64 cursor = 0;
    quint64 query = 0;
    QObject::connect(entries,
                     &QListWidget::currentRowChanged,
                     &dialog,
                     [&](int row)
                     {
                         if (row < 0)
                         {
                             return;
                         }
                         auto const& item = items[row];
                         if (item.markdown)
                         {
                             auto* doc =
                                 new message_markdown(QString::fromStdString(item.body), body->font(), qMax(10, body->viewport()->width() - 4), {});
                             doc->setParent(body);
                             body->setDocument(doc);
                         }
                         else
                         {
                             body->setPlainText(QString::fromStdString(item.body));
                         }
                         file->setVisible(!item.filename.empty());
                     });
    QObject::connect(file,
                     &QPushButton::clicked,
                     &dialog,
                     [&]
                     {
                         auto const item = items[entries->currentRow()];
                         bool const image = item.media_type.starts_with("image/");
                         auto name = QString::fromStdString(item.filename);
                         attachment_dialog preview(room, message, name, image, &dialog, {});
                         QObject::connect(&dialog, &QDialog::finished, &preview, &QDialog::reject);
                         QObject::connect(&preview,
                                          &attachment_dialog::save_requested,
                                          &preview,
                                          [&](qint64, qint64, QString path) { client.get_attachment(room, message, name, path, item.position); });
                         QObject::connect(&client, &client_bridge::attachment_received, &preview, &attachment_dialog::download_finished);
                         quint64 image_query = 0;
                         QObject::connect(&client,
                                          &client_bridge::bundle_image_received,
                                          &preview,
                                          [&](quint64 request, qint64 id, qint64 position, QByteArray bytes, QString error)
                                          {
                                              if (request == image_query && id == message && position == item.position)
                                              {
                                                  preview.set_data(room, message, std::move(bytes), error);
                                              }
                                          });
                         if (image)
                         {
                             image_query = client.get_message_image(room, message, item.position);
                         }
                         preview.exec();
                     });
    QObject::connect(more,
                     &QPushButton::clicked,
                     &dialog,
                     [&]
                     {
                         more->setEnabled(false);
                         query = client.get_bundle(room, message, cursor);
                     });
    QObject::connect(&client,
                     &client_bridge::bundle_received,
                     &dialog,
                     [&](quint64 request, qint64 conversation, qint64 id, chat::forwarded_page page, QString error)
                     {
                         if (request != query || conversation != room || id != message)
                         {
                             return;
                         }
                         query = 0;
                         if (!error.isEmpty())
                         {
                             entries->clear();
                             items.clear();
                             body->setPlainText(error);
                             file->hide();
                             more->hide();
                             return;
                         }
                         for (auto& item : page.items)
                         {
                             auto label = QString::fromStdString(item.author) + QStringLiteral(" · ") +
                                          QDateTime::fromMSecsSinceEpoch(item.created).toString("yyyy-MM-dd hh:mm") + QStringLiteral("  ") +
                                          QString::fromStdString(item.body).section(u'\n', 0, 0).left(80);
                             items.push_back(std::move(item));
                             entries->addItem(label);
                         }
                         if (entries->currentRow() < 0 && !items.empty())
                         {
                             entries->setCurrentRow(0);
                         }
                         cursor = page.next;
                         more->setVisible(cursor != 0);
                         more->setEnabled(cursor != 0);
                     });
    QObject::connect(&chat,
                     &chat_widget::history_discarded,
                     &dialog,
                     [&](qint64 id, qint64 through)
                     {
                         if (id == room && message <= through)
                         {
                             dialog.reject();
                         }
                     });
    QObject::connect(&client,
                     &client_bridge::conversation_changed,
                     &dialog,
                     [&](qint64 id, bool removed)
                     {
                         if (id == room && removed)
                         {
                             dialog.reject();
                         }
                     });
    QObject::connect(&client,
                     &client_bridge::message_updated,
                     &dialog,
                     [&](qint64 id, message_data m, QString)
                     {
                         if (id == room && m.id == message && m.deleted)
                         {
                             dialog.reject();
                         }
                     });
    QObject::connect(&client, &client_bridge::disconnected, &dialog, &QDialog::reject);
    query = client.get_bundle(room, message, 0);
    dialog.exec();
}
