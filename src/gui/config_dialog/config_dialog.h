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

// Qt component of configure dialog for Mozc

#ifndef MOZC_GUI_CONFIG_DIALOG_CONFIG_DIALOG_H_
#define MOZC_GUI_CONFIG_DIALOG_CONFIG_DIALOG_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include <QCheckBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QObject>
#include <QTimer>

#include "client/client_interface.h"
#include "gui/config_dialog/ui_config_dialog.h"
#include "protocol/config.pb.h"

namespace mozc {
namespace gui {

// Product-specific additions to the generated Qt Designer UI.
//
// Keeping this in a thin wrapper lets upstream config_dialog.ui stay close to
// Mozc while still giving Mozkey a scalable place for built-in dictionaries.
// The existing use_t13n_conversion setting is used as the persisted backing
// flag for compatibility. The old Katakana-to-English checkbox is hidden and
// replaced by the clearer English word dictionary entry below.
class MozkeyConfigDialogUi : public Ui::ConfigDialog {
 public:
  void setupUi(QDialog *dialog) {
    Ui::ConfigDialog::setupUi(dialog);

    auto *header = new QFrame(dictionaryTab);
    header->setObjectName(QStringLiteral("builtInDictionaryHeader"));
    header->setFrameShape(QFrame::NoFrame);
    auto *header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(9, 9, 9, 9);
    header_layout->setSpacing(6);

    auto *title = new QLabel(header);
    title->setObjectName(QStringLiteral("builtInDictionaryLabel"));
    title->setText(QString::fromUtf8("内蔵辞書"));
    header_layout->addWidget(title);

    auto *line = new QFrame(header);
    line->setObjectName(QStringLiteral("builtInDictionaryLine"));
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    header_layout->addWidget(line, 1);

    auto *group = new QFrame(dictionaryTab);
    group->setObjectName(QStringLiteral("builtInDictionaryGroup"));
    group->setFrameShape(QFrame::NoFrame);
    auto *group_layout = new QGridLayout(group);
    group_layout->setContentsMargins(24, 9, 24, 9);
    group_layout->setHorizontalSpacing(8);
    group_layout->setVerticalSpacing(6);

    // Move the existing homonym dictionary setting into the common built-in
    // dictionary list instead of showing it in a separate Usage dictionary
    // section. QLayout::addWidget transfers it out of its previous layout.
    group_layout->addWidget(localUsageDictionaryCheckBox, 0, 0);
    localUsageDictionaryCheckBox->setVisible(true);

    englishWordDictionaryCheckBox = new QCheckBox(group);
    englishWordDictionaryCheckBox->setObjectName(
        QStringLiteral("englishWordDictionaryCheckBox"));
    englishWordDictionaryCheckBox->setText(QString::fromUtf8("英単語辞書"));
    englishWordDictionaryCheckBox->setToolTip(
        QString::fromUtf8(
            "日本語の読みから一般的な英単語候補を追加します。例: "
            "ぷろぱてぃ → property"));
    group_layout->addWidget(englishWordDictionaryCheckBox, 1, 0);

    // The old Usage dictionary container is now empty because its checkbox was
    // moved above. Hide the old section and insert the unified built-in list in
    // the same position so the Dictionary tab keeps its familiar ordering.
    int insert_index = dictionaryTabLayout->indexOf(usageDictionaryHeader);
    if (insert_index < 0) {
      insert_index = dictionaryTabLayout->indexOf(specialConversionsHeader);
    }
    if (insert_index < 0) {
      insert_index = dictionaryTabLayout->count();
    }
    dictionaryTabLayout->insertWidget(insert_index, header);
    dictionaryTabLayout->insertWidget(insert_index + 1, group);
    usageDictionaryHeader->setVisible(false);
    usageDictionaryGroup->setVisible(false);

    // Keep the existing persisted setting as a compatibility bridge. The
    // legacy control is hidden so users see one authoritative switch.
    englishWordDictionaryCheckBox->setChecked(
        t13nConversionCheckBox->isChecked());
    QObject::connect(englishWordDictionaryCheckBox, &QCheckBox::toggled,
                     t13nConversionCheckBox, &QCheckBox::setChecked);
    QObject::connect(t13nConversionCheckBox, &QCheckBox::toggled,
                     englishWordDictionaryCheckBox, &QCheckBox::setChecked);
    t13nConversionCheckBox->setVisible(false);
  }

  QCheckBox *englishWordDictionaryCheckBox = nullptr;
};

class ConfigDialog : public QDialog, private MozkeyConfigDialogUi {
  Q_OBJECT;

 public:
  ConfigDialog();

  // Methods defined in the 'slots' section (Qt's extension) will be processed
  // by Qt's moc tool (moc.exe on Windows). Unfortunately, preprocessor macros
  // defined for C/C++ are not automatically passed into the moc tool.
  // For example, you need to call the moc tool with '-D' option as
  // 'moc -DENABLE_FOOBER ...' to make the moc tool aware of the ENABLE_FOOBER
  // macro. http://developer.qt.nokia.com/doc/qt-4.8/moc.html
  // So basically we must not use any #ifdef macro in slot declarations.
  // Otherwise, methods enclosed by "ifdef ENABLE_FOOBER" will be simply ignored
  // by the moc tool and |QObject::connect| against these methods results in
  // failure. See b/5935351 about how we found this issue.
 protected slots:
  virtual void clicked(QAbstractButton *button);
  virtual void ClearUserHistory();
  virtual void ClearUserPrediction();
  virtual void ClearUnusedUserPrediction();
  virtual void EditZenzFeedback();
  virtual void EditUserDictionary();
  virtual void EditKeymap();
  virtual void EditRomanTable();
  virtual void ResetToDefaults();
  virtual void SelectInputModeSetting(int index);
  virtual void SelectLiveConversionSetting(int state);
  virtual void SelectZenzLiveCorrectionSetting(int state);
  virtual void SelectZenzRightContextSetting(int state);
  virtual void SelectZenzFeedbackLearningSetting(int state);
  virtual void SelectAutoConversionSetting(int state);
  virtual void SelectDirectCommitSetting(int state);
  virtual void SelectSuggestionSetting(int state);
  virtual void SelectPreeditColor();
  virtual void SelectRendererAppearanceColor();
  virtual void SelectRendererShadowDirectionPreset();
  virtual void LoadRendererLightAppearance();
  virtual void LoadRendererDarkAppearance();
  virtual void LoadRendererCandidateAppearance();
  virtual void ResetRendererAppearanceControls();
  virtual void UpdateRendererAppearanceControls();
  virtual void LaunchAdministrationDialog();
  virtual void SetMozkeyAsDefaultIme();
  virtual void RestorePreviousDefaultImeSetting();
  virtual void EnableApplyButton();

 protected:
  bool eventFilter(QObject *obj, QEvent *event) override;

 private:
  bool GetConfig(config::Config *config);
  bool SetConfig(const config::Config &config);
  void ConvertToProto(config::Config *config) const;
  void InitializeRendererAppearanceControls();
  void InitializeWindowsImeIconStyleControls();
  void ConvertRendererAppearanceFromProto(const config::Config &config);
  void ConvertRendererAppearanceToProto(config::Config *config) const;
  void ConvertFromProto(const config::Config &config);
  bool Update();
  void Reload();
  void UpdateDependentControls();
  void RecordCurrentStateAsApplied();
  bool IsModified() const;

  std::unique_ptr<client::ClientInterface> client_;
  std::string custom_keymap_table_;
  std::string custom_roman_table_;
  // base_config_ keeps the original config imported from the file including
  // unconfigurable options with the GUI (e.g. composing_timeout_threshold_msec)
  config::Config base_config_;
  config::Config last_applied_config_;
  bool initial_ime_hot_key_disabled_;
  bool initial_startup_enabled_;
  bool suppress_apply_button_update_;
  int initial_preedit_method_;
  bool initial_use_keyboard_to_change_preedit_method_;
  bool initial_use_mode_indicator_;
  int initial_windows_ime_icon_style_;

  bool initial_use_custom_preedit_text_color_;
  uint32_t initial_preedit_text_color_;
  bool initial_use_custom_preedit_background_color_;
  uint32_t initial_preedit_background_color_;
  bool initial_use_custom_preedit_underline_color_;
  uint32_t initial_preedit_underline_color_;

  bool initial_use_custom_preedit_target_text_color_;
  uint32_t initial_preedit_target_text_color_;
  bool initial_use_custom_preedit_target_background_color_;
  uint32_t initial_preedit_target_background_color_;
  bool initial_use_custom_preedit_target_underline_color_;
  uint32_t initial_preedit_target_underline_color_;

  std::map<QString, config::Config::SessionKeymap>
      keymapname_sessionkeymap_map_;
};

}  // namespace gui
}  // namespace mozc
#endif  // MOZC_GUI_CONFIG_DIALOG_CONFIG_DIALOG_H_
