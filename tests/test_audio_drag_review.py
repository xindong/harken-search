from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build_debug" / "audio-drag-review"


def product_fragments():
    source = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
    data_start = source.index("class FileDataObject : public IDataObject {")
    data_end = source.index("\n};", data_start) + len("\n};")
    rect_start = source.index("static RECT WaveformScreenRect(")
    rect_end = source.index("\n}", rect_start) + len("\n}")
    helper_start = source.index("static void SuppressDefaultDragImage(")
    helper_end = source.index("\n}", helper_start) + len("\n}")
    card_start = source.index("        HarkenSearch::AudioDragCard::CardSpec card;", source.index("void StartFileDrag("))
    card_end = source.index("        card.itemCount = (int)files.size();", card_start)
    card_end += len("        card.itemCount = (int)files.size();")
    data = source[data_start:data_end].replace(
        "HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override {",
        "HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override {\n"
        "        wchar_t formatName[128] = {};\n"
        "        if (format) GetClipboardFormatNameW(format->cfFormat, formatName, 128);\n"
        "        std::printf(\"SetData format=%ls requested=%lu actual=%lu release=%d\\n\", formatName, "
        "format ? format->tymed : 0, medium ? medium->tymed : 0, release);\n")
    helper = source[helper_start:helper_end].replace(
        "image.crColorKey = CLR_NONE;",
        "image.crColorKey = CLR_NONE;\n        RecordBitmapValidity(image.hbmpDragImage);")
    return (data + "\n" + helper + "\n" + source[rect_start:rect_end] +
            "\nstatic HarkenSearch::AudioDragCard::CardSpec MakeCardForTest(\n"
            "    const std::vector<std::string>& files, const DragCardOrigin* origin) {\n" +
            source[card_start:card_end] + "\n    return card;\n}\n")


class AudioDragReviewTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        BUILD.mkdir(parents=True, exist_ok=True)
        (BUILD / "product_fragments.h").write_text(product_fragments(), encoding="utf-8")
        cmake = f'''cmake_minimum_required(VERSION 3.16)
project(AudioDragReview LANGUAGES CXX)
add_executable(AudioDragReview "{(ROOT / 'tests/audio_drag_review_harness.cpp').as_posix()}"
    "{(ROOT / 'thirdparty/imgui/imgui.cpp').as_posix()}"
    "{(ROOT / 'thirdparty/imgui/imgui_draw.cpp').as_posix()}"
    "{(ROOT / 'thirdparty/imgui/imgui_tables.cpp').as_posix()}"
    "{(ROOT / 'thirdparty/imgui/imgui_widgets.cpp').as_posix()}")
target_include_directories(AudioDragReview PRIVATE "{BUILD.as_posix()}" "{(ROOT / 'tests').as_posix()}" "{(ROOT / 'thirdparty/imgui').as_posix()}")
target_compile_features(AudioDragReview PRIVATE cxx_std_17)
target_compile_definitions(AudioDragReview PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(AudioDragReview PRIVATE /utf-8)
target_link_libraries(AudioDragReview PRIVATE ole32 shell32 user32 gdi32)
'''
        (BUILD / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
        subprocess.run(["cmake", "-S", str(BUILD), "-B", str(BUILD / "out"),
                        "-G", "Visual Studio 17 2022", "-A", "x64"], check=True)
        subprocess.run(["cmake", "--build", str(BUILD / "out"), "--config", "Debug", "--", "/m:4"], check=True)
        cls.exe = BUILD / "out/Debug/AudioDragReview.exe"

    def run_case(self, name):
        result = subprocess.run([str(self.exe), name], cwd=BUILD, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_viewport_coordinates(self):
        self.run_case("coordinates")

    def test_drag_helper_and_data_ownership(self):
        self.run_case("data")

    def test_waveform_matches_first_file(self):
        self.run_case("waveform")

    def test_wav_fixture_bounds(self):
        self.run_case("wav")


if __name__ == "__main__":
    unittest.main(verbosity=2)
