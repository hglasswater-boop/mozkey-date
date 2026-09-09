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

#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMetaObject>
#include <QPushButton>
#include <QString>
#include <QVariant>
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
  // from ConfigDialog's private API lets the builder stay self-contained.
  QMetaObject::invokeMethod(config_dialog, "EnableApplyButton",
                            Qt::QueuedConnection);
}

}  // namespace

void EnhanceDateFormatControls(QWidget* config_dialog) {
  if (config_dialog == nullptr ||
      config_dialog->findChild<QWidget*>(
          QStringLiteral("dateConversionQuickBuilder")) != nullptr) {
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
        "優先する日付フォーマット（プリセットまたは部品から追加できます。上ほど優先）"));
  }
  if (auto* examples = config_dialog->findChild<QLabel*>(
          QStringLiteral("dateConversionFormatExamplesLabel"))) {
    examples->setText(QString::fromUtf8(
        "詳細編集で使える部品: {YEAR} / {YEAR_NOZERO} / {MONTH} / "
        "{MONTH_NOZERO} / {DATE} / {DATE_NOZERO} / {WEEKDAY} / "
        "{WEEKDAY_LONG}"));
  }
  format_edit->setPlaceholderText(
      QString::fromUtf8("詳細編集（必要な場合のみ）"));
  format_edit->setToolTip(QString::fromUtf8(
      "通常は上のプリセットまたはかんたん作成を使えます。"
      "0サプレスは {MONTH_NOZERO} / {DATE_NOZERO}、曜日は {WEEKDAY} "
      "または {WEEKDAY_LONG} を使用します。"));

  auto* quick_group = new QGroupBox(QString::fromUtf8("かんたん作成"), editor);
  quick_group->setObjectName(QStringLiteral("dateConversionQuickBuilder"));
  auto* quick_layout = new QGridLayout(quick_group);
  quick_layout->setContentsMargins(8, 8, 8, 8);
  quick_layout->setHorizontalSpacing(8);
  quick_layout->setVerticalSpacing(6);

  auto* preset_label = new QLabel(QString::fromUtf8("プリセット"), quick_group);
  auto* preset_combo = new QComboBox(quick_group);
  preset_combo->setObjectName(QStringLiteral("dateConversionPresetComboBox"));
  preset_combo->setMinimumWidth(240);
  const auto add_preset = [preset_combo](const char* label, const char* format) {
    preset_combo->addItem(QString::fromUtf8(label), QString::fromUtf8(format));
  };
  add_preset("2026/09/08", "{YEAR}/{MONTH}/{DATE}");
  add_preset("2026/9/8", "{YEAR_NOZERO}/{MONTH_NOZERO}/{DATE_NOZERO}");
  add_preset("2026-09-08", "{YEAR}-{MONTH}-{DATE}");
  add_preset("2026-9-8", "{YEAR_NOZERO}-{MONTH_NOZERO}-{DATE_NOZERO}");
  add_preset("2026.09.08", "{YEAR}.{MONTH}.{DATE}");
  add_preset("2026.9.8", "{YEAR_NOZERO}.{MONTH_NOZERO}.{DATE_NOZERO}");
  add_preset("2026年9月8日", "{YEAR_NOZERO}年{MONTH_NOZERO}月{DATE_NOZERO}日");
  add_preset("2026年09月08日", "{YEAR}年{MONTH}月{DATE}日");
  add_preset("2026/09/08(火)", "{YEAR}/{MONTH}/{DATE}({WEEKDAY})");
  add_preset("2026/9/8(火)",
             "{YEAR_NOZERO}/{MONTH_NOZERO}/{DATE_NOZERO}({WEEKDAY})");
  add_preset("2026年9月8日(火)",
             "{YEAR_NOZERO}年{MONTH_NOZERO}月{DATE_NOZERO}日({WEEKDAY})");
  add_preset("09/08", "{MONTH}/{DATE}");
  add_preset("9/8", "{MONTH_NOZERO}/{DATE_NOZERO}");
  add_preset("09/08(火)", "{MONTH}/{DATE}({WEEKDAY})");
  add_preset("9/8(火)", "{MONTH_NOZERO}/{DATE_NOZERO}({WEEKDAY})");

  auto* preset_add_button =
      new QPushButton(QString::fromUtf8("追加"), quick_group);
  preset_add_button->setObjectName(
      QStringLiteral("dateConversionPresetAddButton"));
  quick_layout->addWidget(preset_label, 0, 0);
  quick_layout->addWidget(preset_combo, 0, 1, 1, 5);
  quick_layout->addWidget(preset_add_button, 0, 6);

  auto* year_label = new QLabel(QString::fromUtf8("年"), quick_group);
  auto* year_combo = new QComboBox(quick_group);
  year_combo->setObjectName(QStringLiteral("dateConversionYearComboBox"));
  year_combo->addItem(QString::fromUtf8("あり"), true);
  year_combo->addItem(QString::fromUtf8("なし"), false);

  auto* style_label = new QLabel(QString::fromUtf8("形式"), quick_group);
  auto* style_combo = new QComboBox(quick_group);
  style_combo->setObjectName(QStringLiteral("dateConversionStyleComboBox"));
  style_combo->addItem(QStringLiteral("/"), QStringLiteral("/"));
  style_combo->addItem(QStringLiteral("-"), QStringLiteral("-"));
  style_combo->addItem(QStringLiteral("."), QStringLiteral("."));
  style_combo->addItem(QString::fromUtf8("年月日"), QStringLiteral("kanji"));

  auto* padding_label = new QLabel(QString::fromUtf8("数字"), quick_group);
  auto* padding_combo = new QComboBox(quick_group);
  padding_combo->setObjectName(
      QStringLiteral("dateConversionPaddingComboBox"));
  padding_combo->addItem(QString::fromUtf8("0埋め (09/08)"), true);
  padding_combo->addItem(QString::fromUtf8("0サプレス (9/8)"), false);

  auto* weekday_label = new QLabel(QString::fromUtf8("曜日"), quick_group);
  auto* weekday_combo = new QComboBox(quick_group);
  weekday_combo->setObjectName(
      QStringLiteral("dateConversionWeekdayComboBox"));
  weekday_combo->addItem(QString::fromUtf8("なし"), QString());
  weekday_combo->addItem(QString::fromUtf8("(火)"),
                         QStringLiteral("({WEEKDAY})"));
  weekday_combo->addItem(QString::fromUtf8("(火曜日)"),
                         QStringLiteral("({WEEKDAY_LONG})"));
  weekday_combo->addItem(QString::fromUtf8("火"),
                         QStringLiteral("{WEEKDAY}"));
  weekday_combo->addItem(QString::fromUtf8("火曜日"),
                         QStringLiteral("{WEEKDAY_LONG}"));

  quick_layout->addWidget(year_label, 1, 0);
  quick_layout->addWidget(year_combo, 1, 1);
  quick_layout->addWidget(style_label, 1, 2);
  quick_layout->addWidget(style_combo, 1, 3);
  quick_layout->addWidget(padding_label, 1, 4);
  quick_layout->addWidget(padding_combo, 1, 5);
  quick_layout->addWidget(weekday_label, 2, 0);
  quick_layout->addWidget(weekday_combo, 2, 1, 1, 3);

  auto* builder_add_button =
      new QPushButton(QString::fromUtf8("この形式を追加"), quick_group);
  builder_add_button->setObjectName(
      QStringLiteral("dateConversionBuilderAddButton"));
  quick_layout->addWidget(builder_add_button, 2, 4, 1, 3);

  auto* quick_preview = new QLabel(quick_group);
  quick_preview->setObjectName(QStringLiteral("dateConversionQuickPreviewLabel"));
  quick_layout->addWidget(quick_preview, 3, 0, 1, 7);
  quick_layout->setColumnStretch(1, 1);
  quick_layout->setColumnStretch(3, 1);
  quick_layout->setColumnStretch(5, 1);

  editor_layout->insertWidget(0, quick_group);

  const auto build_format = [year_combo, style_combo, padding_combo,
                             weekday_combo]() {
    const bool include_year = year_combo->currentData().toBool();
    const bool zero_pad = padding_combo->currentData().toBool();
    const QString year = zero_pad ? QStringLiteral("{YEAR}")
                                  : QStringLiteral("{YEAR_NOZERO}");
    const QString month = zero_pad ? QStringLiteral("{MONTH}")
                                   : QStringLiteral("{MONTH_NOZERO}");
    const QString day = zero_pad ? QStringLiteral("{DATE}")
                                 : QStringLiteral("{DATE_NOZERO}");
    const QString style = style_combo->currentData().toString();

    QString format;
    if (style == QStringLiteral("kanji")) {
      if (include_year) {
        format += year + QString::fromUtf8("年");
      }
      format += month + QString::fromUtf8("月") + day +
                QString::fromUtf8("日");
    } else {
      if (include_year) {
        format += year + style;
      }
      format += month + style + day;
    }
    format += weekday_combo->currentData().toString();
    return format;
  };

  const auto add_format_if_missing =
      [config_dialog, format_list, format_edit](const QString& input) {
        const QString format = input.trimmed();
        if (format.isEmpty()) {
          return;
        }
        const QList<QListWidgetItem*> existing =
            format_list->findItems(format, Qt::MatchExactly);
        if (!existing.isEmpty()) {
          format_list->setCurrentItem(existing.front());
          format_edit->setText(format);
          return;
        }
        auto* item = new QListWidgetItem(format, format_list);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        format_list->setCurrentItem(item);
        format_edit->setText(format);
        RequestApplyButtonRefresh(config_dialog);
      };

  const auto update_quick_preview = [quick_preview, build_format]() {
    const QString format = build_format();
    quick_preview->setText(
        QString::fromUtf8("プレビュー: %1").arg(PreviewDateFormat(format)));
  };

  const auto update_advanced_preview =
      [format_list, format_edit, preview_label]() {
        QString format;
        if (QListWidgetItem* item = format_list->currentItem()) {
          format = item->text().trimmed();
        } else {
          format = format_edit->text().trimmed();
        }
        preview_label->setText(QString::fromUtf8("プレビュー: %1")
                                   .arg(PreviewDateFormat(format)));
      };

  QObject::connect(preset_add_button, &QPushButton::clicked, config_dialog,
                   [preset_combo, add_format_if_missing]() {
                     add_format_if_missing(
                         preset_combo->currentData().toString());
                   });
  QObject::connect(builder_add_button, &QPushButton::clicked, config_dialog,
                   [build_format, add_format_if_missing]() {
                     add_format_if_missing(build_format());
                   });

  const auto connect_builder_combo =
      [config_dialog, update_quick_preview](QComboBox* combo) {
        QObject::connect(
            combo,
            static_cast<void (QComboBox::*)(int)>(
                &QComboBox::currentIndexChanged),
            config_dialog, [update_quick_preview](int) {
              update_quick_preview();
            });
      };
  connect_builder_combo(year_combo);
  connect_builder_combo(style_combo);
  connect_builder_combo(padding_combo);
  connect_builder_combo(weekday_combo);

  QObject::connect(format_list, &QListWidget::currentRowChanged, config_dialog,
                   [update_advanced_preview](int) {
                     update_advanced_preview();
                   });
  QObject::connect(format_list, &QListWidget::itemChanged, config_dialog,
                   [config_dialog, update_advanced_preview](QListWidgetItem*) {
                     update_advanced_preview();
                     RequestApplyButtonRefresh(config_dialog);
                   });
  QObject::connect(format_edit, &QLineEdit::textChanged, config_dialog,
                   [update_advanced_preview](const QString&) {
                     update_advanced_preview();
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

  update_quick_preview();
  update_advanced_preview();
}

}  // namespace mozc::gui
