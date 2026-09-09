// Copyright 2010-2021, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "gui/config_dialog/date_format_ui_helper.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMetaObject>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

namespace mozc::gui {
namespace {

QString PreviewDateFormat(QString format) {
  if (format.trimmed().isEmpty()) {
    format = QStringLiteral("{YEAR}/{MONTH}/{DATE}");
  }

  // Use 2026-09-08 (Tuesday) for the fixed settings preview. Replace the
  // custom tokens first, then the legacy zero-padded tokens.
  format.replace(QStringLiteral("{YEAR_NOZERO}"), QStringLiteral("2026"));
  format.replace(QStringLiteral("{MONTH_NOZERO}"), QStringLiteral("9"));
  format.replace(QStringLiteral("{DATE_NOZERO}"), QStringLiteral("8"));
  format.replace(QStringLiteral("{WEEKDAY_LONG}"),
                 QString::fromUtf8("火曜日"));
  format.replace(QStringLiteral("{WEEKDAY}"), QString::fromUtf8("火"));
  format.replace(QStringLiteral("{YEAR}"), QStringLiteral("2026"));
  format.replace(QStringLiteral("{MONTH}"), QStringLiteral("09"));
  format.replace(QStringLiteral("{DATE}"), QStringLiteral("08"));
  format.replace(QStringLiteral("{HOUR}"), QStringLiteral("16"));
  format.replace(QStringLiteral("{MINUTE}"), QStringLiteral("02"));
  format.replace(QStringLiteral("{{}"), QStringLiteral("{"));
  return format;
}

void RequestApplyButtonRefresh(QWidget* config_dialog) {
  // EnableApplyButton is a ConfigDialog slot. Keeping this helper decoupled
  // from ConfigDialog's private API lets the date format UI stay self-contained.
  QMetaObject::invokeMethod(config_dialog, "EnableApplyButton",
                            Qt::QueuedConnection);
}

QPushButton* AddPartButton(QGridLayout* layout, QWidget* parent,
                           QLineEdit* format_edit, const QString& label,
                           const QString& part, int row, int column,
                           int column_span = 1) {
  auto* button = new QPushButton(label, parent);
  button->setToolTip(
      QString::fromUtf8("編集欄へ挿入: %1").arg(part));
  layout->addWidget(button, row, column, 1, column_span);
  QObject::connect(button, &QPushButton::clicked, format_edit,
                   [format_edit, part]() {
                     // QLineEdit::insert replaces the current selection and
                     // otherwise inserts exactly at the caret position.
                     format_edit->insert(part);
                     format_edit->setFocus(Qt::OtherFocusReason);
                   });
  return button;
}

}  // namespace

void EnhanceDateFormatControls(QWidget* config_dialog) {
  if (config_dialog == nullptr ||
      config_dialog->findChild<QWidget*>(
          QStringLiteral("dateConversionFormatParts")) != nullptr) {
    return;
  }

  auto* editor = config_dialog->findChild<QWidget*>(
      QStringLiteral("dateConversionFormatEditorWidget"));
  auto* editor_layout = config_dialog->findChild<QVBoxLayout*>(
      QStringLiteral("dateConversionFormatEditorLayout"));
  auto* format_list = config_dialog->findChild<QListWidget*>(
      QStringLiteral("dateConversionFormatListWidget"));
  auto* format_edit = config_dialog->findChild<QLineEdit*>(
      QStringLiteral("dateConversionFormatLineEdit"));
  auto* preview_label = config_dialog->findChild<QLabel*>(
      QStringLiteral("dateConversionFormatPreviewLabel"));
  if (editor == nullptr || editor_layout == nullptr || format_list == nullptr ||
      format_edit == nullptr || preview_label == nullptr) {
    return;
  }

  if (auto* title = config_dialog->findChild<QLabel*>(
          QStringLiteral("dateConversionFormatLabel"))) {
    title->setText(QString::fromUtf8(
        "優先する日付フォーマット（部品をクリックして編集できます。上ほど優先）"));
  }
  if (auto* examples = config_dialog->findChild<QLabel*>(
          QStringLiteral("dateConversionFormatExamplesLabel"))) {
    examples->setText(QString::fromUtf8(
        "部品をクリックすると下の編集欄のカーソル位置へ挿入されます。"
        "挿入後は自由に文字や記号を編集できます。"));
  }

  format_edit->setPlaceholderText(
      QString::fromUtf8("例: {YEAR_NOZERO}/{MONTH_NOZERO}/{DATE_NOZERO}({WEEKDAY})"));
  format_edit->setToolTip(QString::fromUtf8(
      "部品ボタンでひな形を作り、必要な部分だけ直接編集できます。"
      "0サプレスは *_NOZERO、曜日は {WEEKDAY} / {WEEKDAY_LONG} です。"));

  auto* parts_group =
      new QGroupBox(QString::fromUtf8("フォーマット部品"), editor);
  parts_group->setObjectName(QStringLiteral("dateConversionFormatParts"));
  auto* parts_layout = new QGridLayout(parts_group);
  parts_layout->setContentsMargins(8, 8, 8, 8);
  parts_layout->setHorizontalSpacing(6);
  parts_layout->setVerticalSpacing(6);

  auto* hint = new QLabel(
      QString::fromUtf8("クリックすると編集欄へ挿入します。0埋め／0サプレスも部品ごとに選べます。"),
      parts_group);
  hint->setWordWrap(true);
  parts_layout->addWidget(hint, 0, 0, 1, 8);

  parts_layout->addWidget(new QLabel(QString::fromUtf8("年"), parts_group),
                          1, 0);
  AddPartButton(parts_layout, parts_group, format_edit,
                QString::fromUtf8("2026"), QStringLiteral("{YEAR}"), 1, 1);
  AddPartButton(parts_layout, parts_group, format_edit,
                QString::fromUtf8("2026（0サプレス）"),
                QStringLiteral("{YEAR_NOZERO}"), 1, 2, 2);

  parts_layout->addWidget(new QLabel(QString::fromUtf8("月"), parts_group),
                          2, 0);
  AddPartButton(parts_layout, parts_group, format_edit, QStringLiteral("09"),
                QStringLiteral("{MONTH}"), 2, 1);
  AddPartButton(parts_layout, parts_group, format_edit,
                QString::fromUtf8("9（0サプレス）"),
                QStringLiteral("{MONTH_NOZERO}"), 2, 2, 2);

  parts_layout->addWidget(new QLabel(QString::fromUtf8("日"), parts_group),
                          3, 0);
  AddPartButton(parts_layout, parts_group, format_edit, QStringLiteral("08"),
                QStringLiteral("{DATE}"), 3, 1);
  AddPartButton(parts_layout, parts_group, format_edit,
                QString::fromUtf8("8（0サプレス）"),
                QStringLiteral("{DATE_NOZERO}"), 3, 2, 2);

  parts_layout->addWidget(new QLabel(QString::fromUtf8("曜日"), parts_group),
                          4, 0);
  AddPartButton(parts_layout, parts_group, format_edit,
                QString::fromUtf8("火"), QStringLiteral("{WEEKDAY}"), 4, 1);
  AddPartButton(parts_layout, parts_group, format_edit,
                QString::fromUtf8("火曜日"),
                QStringLiteral("{WEEKDAY_LONG}"), 4, 2);
  AddPartButton(parts_layout, parts_group, format_edit,
                QString::fromUtf8("(火)"),
                QStringLiteral("({WEEKDAY})"), 4, 3);
  AddPartButton(parts_layout, parts_group, format_edit,
                QString::fromUtf8("(火曜日)"),
                QStringLiteral("({WEEKDAY_LONG})"), 4, 4, 2);

  parts_layout->addWidget(new QLabel(QString::fromUtf8("区切り"), parts_group),
                          5, 0);
  AddPartButton(parts_layout, parts_group, format_edit, QStringLiteral("/"),
                QStringLiteral("/"), 5, 1);
  AddPartButton(parts_layout, parts_group, format_edit, QStringLiteral("-"),
                QStringLiteral("-"), 5, 2);
  AddPartButton(parts_layout, parts_group, format_edit, QStringLiteral("."),
                QStringLiteral("."), 5, 3);
  AddPartButton(parts_layout, parts_group, format_edit,
                QString::fromUtf8("年"), QString::fromUtf8("年"), 5, 4);
  AddPartButton(parts_layout, parts_group, format_edit,
                QString::fromUtf8("月"), QString::fromUtf8("月"), 5, 5);
  AddPartButton(parts_layout, parts_group, format_edit,
                QString::fromUtf8("日"), QString::fromUtf8("日"), 5, 6);

  parts_layout->addWidget(new QLabel(QString::fromUtf8("記号"), parts_group),
                          6, 0);
  AddPartButton(parts_layout, parts_group, format_edit, QStringLiteral("("),
                QStringLiteral("("), 6, 1);
  AddPartButton(parts_layout, parts_group, format_edit, QStringLiteral(")"),
                QStringLiteral(")"), 6, 2);
  AddPartButton(parts_layout, parts_group, format_edit,
                QString::fromUtf8("空白"), QStringLiteral(" "), 6, 3);

  editor_layout->insertWidget(0, parts_group);

  const auto update_preview = [format_edit, preview_label]() {
    preview_label->setText(
        QString::fromUtf8("プレビュー: %1")
            .arg(PreviewDateFormat(format_edit->text().trimmed())));
  };

  QObject::connect(format_edit, &QLineEdit::textChanged, config_dialog,
                   [update_preview](const QString&) { update_preview(); });

  // The existing ConfigDialog handlers keep the edit box synchronized with
  // the selected list item. Refresh after the row changes so the preview
  // follows that edit box as well.
  QObject::connect(format_list, &QListWidget::currentRowChanged, config_dialog,
                   [update_preview](int) {
                     QMetaObject::invokeMethod(
                         qApp, update_preview, Qt::QueuedConnection);
                   });

  // Existing edit/delete/reorder buttons mutate the list from ConfigDialog.
  // Refresh Apply state after those handlers have completed as well.
  const char* mutation_buttons[] = {
      "dateConversionFormatAddButton", "dateConversionFormatEditButton",
      "dateConversionFormatDeleteButton", "dateConversionFormatUpButton",
      "dateConversionFormatDownButton"};
  for (const char* object_name : mutation_buttons) {
    if (auto* button = config_dialog->findChild<QPushButton*>(
            QString::fromLatin1(object_name))) {
      QObject::connect(button, &QPushButton::clicked, config_dialog,
                       [config_dialog]() {
                         RequestApplyButtonRefresh(config_dialog);
                       });
    }
  }

  update_preview();
}

}  // namespace mozc::gui
