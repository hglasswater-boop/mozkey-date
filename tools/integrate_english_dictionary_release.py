#!/usr/bin/env python3

from pathlib import Path
import re


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    if new in text:
        return
    if old not in text:
        raise RuntimeError(f"anchor not found in {path}: {old[:100]!r}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


# config.proto: 1028 is already used by date-format initialization on current
# main. Allocate new, non-overlapping field numbers for English assistance.
p = Path("src/protocol/config.proto")
text = p.read_text(encoding="utf-8")
if "use_english_word_dictionary" not in text:
    for field_number in (1029, 1030):
        if re.search(rf"=\s*{field_number}\b", text):
            raise RuntimeError(
                f"config.proto field number {field_number} is already in use"
            )
    anchor = "  optional bool use_spelling_correction = 88 [default = true];\n"
    block = (
        anchor
        + "\n"
        + "  // Mozkey English input assistance. This is independent from\n"
        + "  // Katakana-to-English conversion and Japanese reading correction.\n"
        + "  optional bool use_english_word_dictionary = 1029 [default = true];\n"
        + "  optional bool use_english_spelling_correction = 1030 [default = true];\n"
    )
    if anchor not in text:
        raise RuntimeError("use_spelling_correction anchor not found")
    p.write_text(text.replace(anchor, block, 1), encoding="utf-8")


# Add product-specific built-in dictionary controls without replacing the
# current main header, so recent date/renderer declarations stay intact.
p = Path("src/gui/config_dialog/config_dialog.h")
text = p.read_text(encoding="utf-8")
if "class MozkeyConfigDialogUi" not in text:
    include_anchor = "#include <QTimer>\n"
    extra_includes = (
        "#include <QTimer>\n"
        "#include <QCheckBox>\n"
        "#include <QFrame>\n"
        "#include <QGridLayout>\n"
        "#include <QHBoxLayout>\n"
        "#include <QLabel>\n"
    )
    if include_anchor not in text:
        raise RuntimeError("Qt include anchor not found in config_dialog.h")
    text = text.replace(include_anchor, extra_includes, 1)

    class_anchor = "class ConfigDialog : public QDialog, private Ui::ConfigDialog {\n"
    wrapper = '''// Product-specific additions to the generated Qt Designer UI.
// Keeping this in a thin wrapper lets upstream config_dialog.ui stay close to
// Mozc while giving Mozkey a scalable place for built-in dictionaries.
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

    group_layout->addWidget(localUsageDictionaryCheckBox, 0, 0);
    localUsageDictionaryCheckBox->setVisible(true);

    englishWordDictionaryCheckBox = new QCheckBox(group);
    englishWordDictionaryCheckBox->setObjectName(
        QStringLiteral("englishWordDictionaryCheckBox"));
    englishWordDictionaryCheckBox->setText(QString::fromUtf8("英単語辞書"));
    englishWordDictionaryCheckBox->setToolTip(
        QString::fromUtf8(
            "英字入力中に英単語の補完候補を表示します。例: prope → property"));
    group_layout->addWidget(englishWordDictionaryCheckBox, 1, 0);

    englishSpellingCorrectionCheckBox = new QCheckBox(group);
    englishSpellingCorrectionCheckBox->setObjectName(
        QStringLiteral("englishSpellingCorrectionCheckBox"));
    englishSpellingCorrectionCheckBox->setText(
        QString::fromUtf8("スペルミスの訂正候補"));
    englishSpellingCorrectionCheckBox->setToolTip(
        QString::fromUtf8(
            "英単語の綴りが違うときに正しい候補を表示します。例: recieve → receive"));
    englishSpellingCorrectionCheckBox->setContentsMargins(18, 0, 0, 0);
    group_layout->addWidget(englishSpellingCorrectionCheckBox, 2, 0);

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

    englishSpellingCorrectionCheckBox->setEnabled(
        englishWordDictionaryCheckBox->isChecked());
    QObject::connect(englishWordDictionaryCheckBox, &QCheckBox::toggled,
                     englishSpellingCorrectionCheckBox, &QCheckBox::setEnabled);
  }

  QCheckBox *englishWordDictionaryCheckBox = nullptr;
  QCheckBox *englishSpellingCorrectionCheckBox = nullptr;
};

'''
    if class_anchor not in text:
        raise RuntimeError("ConfigDialog inheritance anchor not found")
    text = text.replace(
        class_anchor,
        wrapper
        + "class ConfigDialog : public QDialog, private MozkeyConfigDialogUi {\n",
        1,
    )
    p.write_text(text, encoding="utf-8")


# Persist the two new controls through the normal ConfigDialog apply path.
replace_once(
    "src/gui/config_dialog/config_dialog.cc",
    "  SET_CHECKBOX(spellingCorrectionCheckBox, use_spelling_correction);\n",
    "  SET_CHECKBOX(spellingCorrectionCheckBox, use_spelling_correction);\n"
    "  englishWordDictionaryCheckBox->setChecked(\n"
    "      config.use_english_word_dictionary());\n"
    "  englishSpellingCorrectionCheckBox->setChecked(\n"
    "      config.use_english_spelling_correction());\n"
    "  englishSpellingCorrectionCheckBox->setEnabled(\n"
    "      config.use_english_word_dictionary());\n",
)
replace_once(
    "src/gui/config_dialog/config_dialog.cc",
    "  GET_CHECKBOX(spellingCorrectionCheckBox, use_spelling_correction);\n",
    "  GET_CHECKBOX(spellingCorrectionCheckBox, use_spelling_correction);\n"
    "  config->set_use_english_word_dictionary(\n"
    "      englishWordDictionaryCheckBox->isChecked());\n"
    "  config->set_use_english_spelling_correction(\n"
    "      englishSpellingCorrectionCheckBox->isChecked());\n",
)


# Register the new rewriter while preserving current main's date postprocessor.
replace_once(
    "src/rewriter/rewriter.cc",
    '#include "rewriter/english_variants_rewriter.h"\n',
    '#include "rewriter/english_variants_rewriter.h"\n'
    '#include "rewriter/english_word_dictionary_rewriter.h"\n',
)
replace_once(
    "src/rewriter/rewriter.cc",
    "  AddRewriter(std::make_unique<EnglishVariantsRewriter>(pos_matcher));\n",
    "  AddRewriter(std::make_unique<EnglishVariantsRewriter>(pos_matcher));\n"
    "  AddRewriter(std::make_unique<EnglishWordDictionaryRewriter>());\n",
)


# Avoid full edit-distance work for every 160k+ dictionary entry. Completion
# still uses the full dictionary. Spelling correction focuses on common tiers
# and words within the permitted edit-distance length delta.
p = Path("src/rewriter/english_word_dictionary_rewriter.cc")
text = p.read_text(encoding="utf-8")
if "kMaxSpellingTier" not in text:
    anchor = "constexpr size_t kPrefixScanLimit = 512;\n"
    if anchor not in text:
        raise RuntimeError("English dictionary constants anchor not found")
    text = text.replace(
        anchor,
        anchor + "constexpr uint8_t kMaxSpellingTier = 60;\n",
        1,
    )
    loop_anchor = (
        "  for (const EnglishWordData& entry : kEnglishWordDictionary) {\n"
        "    const int distance =\n"
        "        BoundedDamerauLevenshtein(input, entry.word, max_distance);\n"
    )
    loop_replacement = (
        "  for (const EnglishWordData& entry : kEnglishWordDictionary) {\n"
        "    if (entry.tier > kMaxSpellingTier ||\n"
        "        std::abs(static_cast<int>(entry.word.size()) -\n"
        "                 static_cast<int>(input.size())) > max_distance) {\n"
        "      continue;\n"
        "    }\n"
        "    const int distance =\n"
        "        BoundedDamerauLevenshtein(input, entry.word, max_distance);\n"
    )
    if loop_anchor not in text:
        raise RuntimeError("Spelling scan loop anchor not found")
    p.write_text(text.replace(loop_anchor, loop_replacement, 1), encoding="utf-8")


# Make the feature a permanent Windows CI contract rather than a one-off
# diagnostic workflow.
p = Path(".github/workflows/windows.yaml")
text = p.read_text(encoding="utf-8")
if "Test English word dictionary" not in text:
    anchor = "      - name: Build x64 package\n"
    block = '''      - name: Test English word dictionary
        shell: cmd
        working-directory: .\\src
        env:
          ANDROID_NDK_HOME: ""
        run: |
          bazelisk test --config release_build --test_output=errors --disk_cache=.bazel-disk-cache --experimental_disk_cache_gc_max_size=4G --experimental_disk_cache_gc_idle_delay=0s //rewriter:english_word_dictionary_rewriter_test

'''
    if anchor not in text:
        raise RuntimeError("Windows package step anchor not found")
    p.write_text(text.replace(anchor, block + anchor, 1), encoding="utf-8")
