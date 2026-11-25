#ifndef CHAT_QT_SRC_MESSAGE_MARKDOWN_HPP
#define CHAT_QT_SRC_MESSAGE_MARKDOWN_HPP

#include "message_data.hpp"
#include <QTextDocument>

class message_markdown final : public QTextDocument
{
   public:
    message_markdown(QString const& source, QFont const& font, int width, QList<mention_data> const& mentions);
    QString code_at(QPointF point) const;
    QString code_for_link(QUrl const& url) const;

   protected:
    QVariant loadResource(int type, QUrl const& name) override;

   private:
    void format_code_blocks(QString const& source);
};

#endif
