#include <cstdio>
#include <string>

#include "../src/HarkenMeta.h"

static int failed = 0;
static int passed = 0;

static void CHECK(bool condition, const char* desc) {
    if (condition) {
        ++passed;
        std::printf("  PASS: %s\n", desc);
    } else {
        ++failed;
        std::printf("  FAIL: %s\n", desc);
    }
}

int main() {
    std::printf("=== HarkenMeta Tests ===\n");

    const std::string json = R"json({"category":"Weapons","function":"Impact","source":"ai"})json";
    {
        const auto values = HarkenMeta::Parse(json);
        CHECK(values.category == "Weapons", "parse category");
        CHECK(values.function == "Impact", "parse function");
        CHECK(values.source == "ai", "parse source");
        CHECK(HarkenMeta::SearchText(json) == "Weapons Impact", "search text uses values only");
        CHECK(!HarkenMeta::IsManual(json), "ai source is not manual");
    }

    {
        HarkenMeta::Update update;
        update.setCategory = true;
        update.category = "Ambience";
        const std::string merged = HarkenMeta::Merge(json, update);
        CHECK(merged.find("\"category\":\"Ambience\"") != std::string::npos, "merge replaces category");
        CHECK(merged.find("\"function\":\"Impact\"") != std::string::npos, "merge keeps function");
        CHECK(merged.find("\"source\":\"ai\"") != std::string::npos, "merge keeps source");
    }

    {
        HarkenMeta::Update update;
        update.setCategory = true;
        update.category = "";
        update.setFunction = true;
        update.function = "";
        update.setSource = true;
        update.source = "manual";
        CHECK(HarkenMeta::Merge(json, update).empty(), "clearing both values stores an empty column");
    }

    {
        HarkenMeta::Update update;
        update.setSource = true;
        update.source = "manual";
        const std::string merged = HarkenMeta::Merge(json, update);
        CHECK(HarkenMeta::IsManual(merged), "manual source round-trips");
    }

    {
        const std::string withUnknown =
            R"json({"category":"UI","function":"Click","source":"ai","future":"keep"})json";
        HarkenMeta::Update update;
        update.setFunction = true;
        update.function = "Confirm";
        const std::string merged = HarkenMeta::Merge(withUnknown, update);
        CHECK(merged.find("\"future\":\"keep\"") != std::string::npos, "unknown keys are preserved");
    }

    {
        HarkenMeta::Update update;
        update.setCategory = true;
        update.category = "He said \"stop\"\\now";
        update.setFunction = true;
        update.function = "Line\nbreak";
        update.setSource = true;
        update.source = "ai";
        const std::string withQuotes = HarkenMeta::Merge("", update);
        const auto values = HarkenMeta::Parse(withQuotes);
        CHECK(values.category == "He said \"stop\"\\now", "quotes and backslash round-trip");
        CHECK(values.function == "Line\nbreak", "newline round-trip");
    }

    {
        const std::string existing =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<BWFXML>\n"
            "  <USER>\n"
            "    <Category>Foley</Category>\n"
            "  </USER>\n"
            "</BWFXML>\n";
        HarkenMeta::Values values;
        values.category = "Footsteps";
        values.function = "Walk";
        const std::string merged = HarkenMeta::MergeIntoIXML(existing, values);
        CHECK(merged.find("<Category>Foley</Category>") != std::string::npos,
              "existing USER value preserved");
        CHECK(merged.find("<HARKEN>") != std::string::npos, "HARKEN inserted");
        CHECK(merged.find("<CATEGORY>Footsteps</CATEGORY>") != std::string::npos,
              "HARKEN category inserted");
        CHECK(merged.find("<FUNCTION>Walk</FUNCTION>") != std::string::npos,
              "HARKEN function inserted");
        CHECK(merged.find("HARKEN") < merged.find("</USER>"), "HARKEN stays inside USER");
    }

    {
        HarkenMeta::Values values;
        values.category = "A&B";
        values.function = "<x>";
        const std::string xml = HarkenMeta::BuildIXML(values);
        CHECK(xml.find("<CATEGORY>A&amp;B</CATEGORY>") != std::string::npos, "XML escaping");
        CHECK(xml.find("<FUNCTION>&lt;x&gt;</FUNCTION>") != std::string::npos, "XML angle escaping");
    }

    {
        HarkenMeta::Values values;
        const std::string existing =
            "<BWFXML>\n  <USER>\n    <HARKEN>\n      <CATEGORY>Old</CATEGORY>\n    </HARKEN>\n  </USER>\n</BWFXML>\n";
        const std::string removed = HarkenMeta::MergeIntoIXML(existing, values);
        CHECK(removed.find("HARKEN") == std::string::npos, "empty values remove HARKEN");
        CHECK(removed.find("<USER>") != std::string::npos, "USER survives removal");
    }

    {
        // A vendor iXML may carry the empty section as "<USER />"; merging must
        // fill it instead of leaving an empty USER next to a new one.
        HarkenMeta::Values values;
        values.category = "Weapons";
        const std::string existing =
            "<BWFXML>\n  <USER />\n</BWFXML>\n";
        const std::string merged = HarkenMeta::MergeIntoIXML(existing, values);
        size_t userCount = 0;
        for (size_t at = merged.find("<USER"); at != std::string::npos;
             at = merged.find("<USER", at + 5)) ++userCount;
        CHECK(userCount == 1, "spaced self-closing USER is filled, not duplicated");
        CHECK(merged.find("<CATEGORY>Weapons</CATEGORY>") != std::string::npos,
              "HARKEN written into the filled USER");
    }

    {
        // The same holds for an empty "<HARKEN/>" already inside USER.
        HarkenMeta::Values values;
        values.category = "Weapons";
        const std::string existing =
            "<BWFXML>\n  <USER>\n    <HARKEN/>\n  </USER>\n</BWFXML>\n";
        const std::string merged = HarkenMeta::MergeIntoIXML(existing, values);
        size_t harkenCount = 0;
        for (size_t at = merged.find("<HARKEN"); at != std::string::npos;
             at = merged.find("<HARKEN", at + 7)) ++harkenCount;
        CHECK(harkenCount == 1, "self-closing HARKEN is replaced, not duplicated");
        CHECK(merged.find("<CATEGORY>Weapons</CATEGORY>") != std::string::npos,
              "replaced HARKEN carries the new value");
    }

    {
        HarkenMeta::Values values;
        values.category = "Weapons";
        const std::string existing =
            "<BWFXML>\n  <USER>\n    <HARKEN />\n  </USER>\n</BWFXML>\n";
        const std::string merged = HarkenMeta::MergeIntoIXML(existing, values);
        size_t harkenCount = 0;
        for (size_t at = merged.find("<HARKEN"); at != std::string::npos;
             at = merged.find("<HARKEN", at + 7)) ++harkenCount;
        CHECK(harkenCount == 1, "spaced self-closing HARKEN is replaced");
    }

    {
        HarkenMeta::Values values;
        values.function = "OnlyFunction";
        const std::string xml = HarkenMeta::MergeIntoIXML("", values);
        CHECK(xml.find("<CATEGORY>") == std::string::npos, "empty category key omitted");
        CHECK(xml.find("<FUNCTION>OnlyFunction</FUNCTION>") != std::string::npos,
              "non-empty function written");
    }

    std::printf("\n=== HarkenMeta: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
