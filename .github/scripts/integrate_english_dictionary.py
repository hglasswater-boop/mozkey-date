from pathlib import Path
import subprocess

OLD = "origin/feature/english-word-dictionary"


def old_bytes(path: str) -> bytes:
    return subprocess.check_output(["git", "show", f"{OLD}:{path}"])


def old_text(path: str) -> str:
    return old_bytes(path).decode("utf-8")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one anchor, found {count}")
    return text.replace(old, new, 1)


def extract_rule(text: str, rule: str, name: str) -> str:
    marker = f'{rule}(\n    name = "{name}",'
    start = text.find(marker)
    if start < 0:
        raise RuntimeError(f"Cannot find {rule} {name}")
    open_pos = text.find("(", start)
    depth = 0
    quote = None
    escaped = False
    for i in range(open_pos, len(text)):
        c = text[i]
        if quote is not None:
            if escaped:
                escaped = False
            elif c == "\\":
                escaped = True
            elif c == quote:
                quote = None
            continue
        if c in ('"', "'"):
            quote = c
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                end = i + 1
                while end < len(text) and text[end] in "\r\n":
                    end += 1
                return text[start:end]
    raise RuntimeError(f"Unterminated {rule} {name}")


new_files = [
    "src/rewriter/english_word_dictionary_ESDB_LICENSE.txt",
    "src/rewriter/english_word_dictionary_data.inc",
    "src/rewriter/english_word_dictionary_rewriter.cc",
    "src/rewriter/english_word_dictionary_rewriter.h",
    "src/rewriter/english_word_dictionary_rewriter_test.cc",
    "src/rewriter/gen_english_word_dictionary.py",
]
for path in new_files:
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(old_bytes(path))

# Current main owns 1028 for date_conversion_custom_formats_initialized.
# Keep date and English settings independent with fresh IDs.
config_path = Path("src/protocol/config.proto")
config = config_path.read_text(encoding="utf-8")
config = replace_once(
    config,
    "  optional bool use_spelling_correction = 88 [default = true];\n",
    "  optional bool use_spelling_correction = 88 [default = true];\n\n"
    "  // Mozkey English input assistance. This is independent from\n"
    "  // Katakana-to-English conversion and Japanese reading correction.\n"
    "  optional bool use_english_word_dictionary = 1029 [default = true];\n"
    "  optional bool use_english_spelling_correction = 1030 [default = true];\n",
    "config.proto English settings",
)
if "date_conversion_custom_formats_initialized = 1028" not in config:
    raise RuntimeError("Current main date format initialization field disappeared")
config_path.write_text(config, encoding="utf-8")

# Thin product wrapper around generated Qt UI.
header_path = Path("src/gui/config_dialog/config_dialog.h")
header = header_path.read_text(encoding="utf-8")
qt_includes = (
    "#include <QCheckBox>\n"
    "#include <QFrame>\n"
    "#include <QGridLayout>\n"
    "#include <QHBoxLayout>\n"
    "#include <QLabel>\n"
)
header = replace_once(
    header,
    "#include <QObject>\n",
    qt_includes + "#include <QObject>\n",
    "config_dialog.h Qt includes",
)
wrapper = r'''// Product-specific additions to the generated Qt Designer UI.
// English input assistance is independent from Katakana-to-English conversion:
// it completes ASCII input and can suggest corrected English spellings.
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
class_anchor = "class ConfigDialog : public QDialog, private Ui::ConfigDialog {\n"
header = replace_once(
    header,
    class_anchor,
    wrapper + "class ConfigDialog : public QDialog, private MozkeyConfigDialogUi {\n",
    "config_dialog.h UI wrapper",
)
header_path.write_text(header, encoding="utf-8")

# Persist English settings without replacing current main dialog logic.
cc_path = Path("src/gui/config_dialog/config_dialog.cc")
cc = cc_path.read_text(encoding="utf-8")
cc = replace_once(
    cc,
    "  SET_CHECKBOX(spellingCorrectionCheckBox, use_spelling_correction);\n",
    "  SET_CHECKBOX(spellingCorrectionCheckBox, use_spelling_correction);\n"
    "  englishWordDictionaryCheckBox->setChecked(\n"
    "      config.use_english_word_dictionary());\n"
    "  englishSpellingCorrectionCheckBox->setChecked(\n"
    "      config.use_english_spelling_correction());\n"
    "  englishSpellingCorrectionCheckBox->setEnabled(\n"
    "      config.use_english_word_dictionary());\n",
    "config_dialog.cc load English settings",
)
cc = replace_once(
    cc,
    "  GET_CHECKBOX(spellingCorrectionCheckBox, use_spelling_correction);\n",
    "  GET_CHECKBOX(spellingCorrectionCheckBox, use_spelling_correction);\n"
    "  config->set_use_english_word_dictionary(\n"
    "      englishWordDictionaryCheckBox->isChecked());\n"
    "  config->set_use_english_spelling_correction(\n"
    "      englishSpellingCorrectionCheckBox->isChecked());\n",
    "config_dialog.cc save English settings",
)
cc_path.write_text(cc, encoding="utf-8")

# Reuse validated Bazel rules and wire the library only into aggregate rewriter.
build_path = Path("src/rewriter/BUILD.bazel")
build = build_path.read_text(encoding="utf-8")
old_build = old_text("src/rewriter/BUILD.bazel")
lib_rule = extract_rule(old_build, "mozc_cc_library", "english_word_dictionary_rewriter")
test_rule = extract_rule(old_build, "mozc_cc_test", "english_word_dictionary_rewriter_test")
english_variants_marker = 'mozc_cc_library(\n    name = "english_variants_rewriter",'
if english_variants_marker not in build:
    raise RuntimeError("Cannot find english_variants_rewriter insertion point")
build = build.replace(
    english_variants_marker,
    lib_rule + test_rule + english_variants_marker,
    1,
)
aggregate = extract_rule(build, "mozc_cc_library", "rewriter")
if '":english_word_dictionary_rewriter",' not in aggregate:
    aggregate_new = replace_once(
        aggregate,
        '        ":english_variants_rewriter",\n',
        '        ":english_variants_rewriter",\n'
        '        ":english_word_dictionary_rewriter",\n',
        "aggregate rewriter English dependency",
    )
    build = build.replace(aggregate, aggregate_new, 1)
build_path.write_text(build, encoding="utf-8")

# Register immediately after Mozc English variants rewriting.
rw_path = Path("src/rewriter/rewriter.cc")
rw = rw_path.read_text(encoding="utf-8")
rw = replace_once(
    rw,
    '#include "rewriter/english_variants_rewriter.h"\n',
    '#include "rewriter/english_variants_rewriter.h"\n'
    '#include "rewriter/english_word_dictionary_rewriter.h"\n',
    "rewriter.cc include",
)
rw = replace_once(
    rw,
    "  AddRewriter(std::make_unique<EnglishVariantsRewriter>(pos_matcher));\n",
    "  AddRewriter(std::make_unique<EnglishVariantsRewriter>(pos_matcher));\n"
    "  AddRewriter(std::make_unique<EnglishWordDictionaryRewriter>());\n",
    "rewriter.cc registration",
)
rw_path.write_text(rw, encoding="utf-8")

tracked = subprocess.check_output(["git", "status", "--short"], text=True)
if "date-dictionary" in tracked.lower():
    raise RuntimeError("Unexpected date-dictionary content detected")
print(tracked)
