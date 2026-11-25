#include "bundle_dialog.hpp"
#include "attachment_dialog.hpp"
#include "chat_widget.hpp"
#include "client_bridge.hpp"
#include "message_markdown.hpp"
#include "message_links.hpp"
#include <QApplication>
#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QPushButton>
#include <QScrollArea>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace
{
class snapshot_body final : public QTextBrowser
{
   protected:
    void paintEvent(QPaintEvent* event) override
    {
        QTextBrowser::paintEvent(event);
        if (auto* doc = dynamic_cast<message_markdown*>(document()))
        {
            QPainter painter(viewport());
            painter.translate(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
            doc->draw_quote_guides(painter);
        }
    }
};

void show_attachment(client_bridge& client, QDialog& dialog, qint64 room, qint64 message, qint64 position, QString const& name, bool image)
{
    attachment_dialog preview(room, message, name, image, &dialog, {});
    QObject::connect(&dialog, &QDialog::finished, &preview, &QDialog::reject);
    QObject::connect(&preview,
                     &attachment_dialog::save_requested,
                     &preview,
                     [&](qint64, qint64, QString path) { client.get_attachment(room, message, name, path, position); });
    QObject::connect(&client, &client_bridge::attachment_received, &preview, &attachment_dialog::download_finished);
    quint64 image_query = 0;
    QObject::connect(&client,
                     &client_bridge::bundle_image_received,
                     &preview,
                     [&](quint64 request, qint64 id, qint64 received_position, QByteArray bytes, QString error)
                     {
                         if (request == image_query && id == message && received_position == position)
                         {
                             preview.set_data(room, message, std::move(bytes), error);
                         }
                     });
    if (image)
    {
        image_query = client.get_message_image(room, message, position);
    }
    preview.exec();
}

void append_entry(QVBoxLayout& stream, chat::forwarded_item const& item, QDialog& dialog, client_bridge& client, qint64 room, qint64 message)
{
    auto* entry = new QWidget;
    entry->setObjectName(QStringLiteral("bundleEntry"));
    auto* layout = new QVBoxLayout(entry);
    layout->setContentsMargins(0, 6, 0, 8);
    layout->setSpacing(4);
    auto* heading = new QHBoxLayout;
    auto* author = new QLabel(QString::fromStdString(item.author));
    author->setTextFormat(Qt::PlainText);
    author->setObjectName(QStringLiteral("snapshotAuthor"));
    auto* date = new QLabel(QDateTime::fromMSecsSinceEpoch(item.created).toString("yyyy-MM-dd hh:mm"));
    date->setObjectName(QStringLiteral("snapshotTime"));
    heading->addWidget(author, 1);
    heading->addWidget(date);
    layout->addLayout(heading);
    auto const source = QString::fromStdString(item.body);
    if (!item.filename.empty())
    {
        auto const name = QString::fromStdString(item.filename);
        bool const image = item.media_type.starts_with("image/");
        auto* filename = new QLabel(name);
        filename->setTextFormat(Qt::PlainText);
        filename->setWordWrap(true);
        filename->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(filename);
        auto* file = new QPushButton(image ? QStringLiteral("查看图片") : QStringLiteral("查看文件"));
        file->setObjectName(QStringLiteral("snapshotAttachment"));
        layout->addWidget(file, 0, Qt::AlignLeft);
        QObject::connect(file,
                         &QPushButton::clicked,
                         &dialog,
                         [&, position = item.position, name, image, room, message]
                         { show_attachment(client, dialog, room, message, position, name, image); });
    }
    else
    {
        auto* body = new snapshot_body;
        body->setObjectName(QStringLiteral("snapshotBody"));
        body->setOpenLinks(false);
        body->setFrameShape(QFrame::NoFrame);
        body->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        body->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        if (item.markdown)
        {
            auto* doc = new message_markdown(source, body->font(), dialog.width() - 64, {});
            doc->setParent(body);
            body->setDocument(doc);
        }
        else
        {
            body->setPlainText(source);
            QTextCursor cursor(body->document());
            for (auto const& range : message_links(source))
            {
                cursor.setPosition(range.start);
                cursor.setPosition(range.start + range.length, QTextCursor::KeepAnchor);
                cursor.mergeCharFormat(range.format);
            }
        }
        body->document()->setDocumentMargin(0);
        auto size_to_content = [body](QSizeF size) { body->setFixedHeight(qCeil(size.height()) + 2); };
        QObject::connect(body->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, body, size_to_content);
        size_to_content(body->document()->size());
        QObject::connect(body,
                         &QTextBrowser::anchorClicked,
                         body,
                         [body](QUrl const& url)
                         {
                             if (auto* doc = dynamic_cast<message_markdown*>(body->document()))
                             {
                                 auto code = doc->code_for_link(url);
                                 if (!code.isNull())
                                 {
                                     QApplication::clipboard()->setText(code);
                                     return;
                                 }
                             }
                             auto allowed = http_message_url(url.toString(QUrl::FullyEncoded));
                             if (!allowed.isEmpty())
                             {
                                 QDesktopServices::openUrl(allowed);
                             }
                         });
        body->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(body,
                         &QWidget::customContextMenuRequested,
                         body,
                         [body, source, &dialog](QPoint point)
                         {
                             QMenu menu(body);
                             QObject::connect(&dialog, &QDialog::finished, &menu, &QMenu::close);
                             auto selected = body->textCursor().selectedText();
                             auto* selection = selected.isEmpty() ? nullptr : menu.addAction(QStringLiteral("复制选中文字"));
                             menu.addAction(QStringLiteral("复制原文"));
                             auto* doc = dynamic_cast<message_markdown*>(body->document());
                             auto code = doc ? doc->code_at(point) : QString{};
                             menu.addSeparator();
                             auto* block = code.isNull() ? nullptr : menu.addAction(QStringLiteral("复制代码"));
                             auto const url = http_message_url(body->anchorAt(point));
                             auto* open_link = url.isEmpty() ? nullptr : menu.addAction(QStringLiteral("打开链接"));
                             auto* copy_link = url.isEmpty() ? nullptr : menu.addAction(QStringLiteral("复制链接"));
                             auto* action = menu.exec(body->viewport()->mapToGlobal(point));
                             if (!action)
                             {
                                 return;
                             }
                             if (action == open_link)
                             {
                                 QDesktopServices::openUrl(url);
                             }
                             else
                             {
                                 QApplication::clipboard()->setText(action == copy_link   ? url.toString(QUrl::FullyEncoded)
                                                                    : action == selection ? selected
                                                                    : action == block     ? code
                                                                                          : source);
                             }
                         });
        layout->addWidget(body);
    }
    stream.addWidget(entry);
}
}    // namespace

void show_bundle(client_bridge& client, chat_widget& chat, qint64 room, qint64 message, QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("聊天记录"));
    dialog.resize(560, 540);
    auto* layout = new QVBoxLayout(&dialog);
    auto* title = new QLabel(dialog.windowTitle());
    title->setObjectName(QStringLiteral("dialogTitle"));
    layout->addWidget(title);
    auto* scroller = new QScrollArea;
    scroller->setWidgetResizable(true);
    scroller->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroller->setFrameShape(QFrame::NoFrame);
    auto* contents = new QWidget;
    auto* stream = new QVBoxLayout(contents);
    stream->setContentsMargins(8, 0, 8, 0);
    stream->setAlignment(Qt::AlignTop);
    scroller->setWidget(contents);
    layout->addWidget(scroller);
    auto* more = new QPushButton(QStringLiteral("加载更多"));
    more->hide();
    layout->addWidget(more);
    qint64 cursor = 0;
    quint64 query = 0;
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
                             scroller->hide();
                             auto* failure = new QLabel(error);
                             failure->setTextFormat(Qt::PlainText);
                             layout->addWidget(failure);
                             more->hide();
                             return;
                         }
                         for (auto const& item : page.items)
                         {
                             append_entry(*stream, item, dialog, client, room, message);
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
