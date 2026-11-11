"""Exercise the real main.cpp deletion entry with a recording GL backend.

The policy unit test alone cannot detect an unused deferred-delete queue.
Compile the actual two function bodies so the frame-lifetime check also covers
the connection from cache eviction/replacement to the end-of-frame flush.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def function_body(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
        end += 1
    return source[start:end]


class MiniWaveformDeleteWiring(unittest.TestCase):
    def test_actual_entry_keeps_draw_ids_alive_until_flush(self):
        compiler = shutil.which(os.environ.get('CXX', 'clang++')) or shutil.which('g++')
        if os.name != 'nt':
            self.assertIsNotNone(compiler, 'C++ compiler required; do not skip lifetime regression')
        source = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
        translation_unit = r'''
#include "src/MiniWaveformPresentation.h"
#include <cstdio>
using GLuint = unsigned int;
struct State { int coverWall; };
static State state{};
static State* g_appState = &state;
static std::vector<unsigned int> deleted;
static MiniWaveformPresentation::DeferredTextureDeletes g_deferredMiniWaveTextureDeletes;
namespace aura { namespace cover_wall {
void CullTextureMirrorsMatchingNonCoverTexture(int&, GLuint) {}
} }
void glDeleteTextures(int, const GLuint* id) { deleted.push_back(*id); }
'''
        translation_unit += function_body(source, 'static void DeleteMiniWaveTexture(')
        translation_unit += '\n' + function_body(source, 'void FlushDeferredMiniWaveTextureDeletes()')
        translation_unit += r'''
int main() {
    GLuint oldTexture = 42, otherTexture = 43, empty = 0;
    const GLuint queuedDrawIds[] = {oldTexture, otherTexture};
    DeleteMiniWaveTexture(oldTexture);
    DeleteMiniWaveTexture(otherTexture);
    DeleteMiniWaveTexture(oldTexture);
    DeleteMiniWaveTexture(empty);
    if (oldTexture || otherTexture || !deleted.empty()) {
        puts("FAIL: texture referenced by pending draw deleted before rendering");
        return 1;
    }
    // Submit both queued Image commands before the frame-tail cleanup.
    if (queuedDrawIds[0] != 42 || queuedDrawIds[1] != 43) return 2;
    FlushDeferredMiniWaveTextureDeletes();
    if (deleted != std::vector<unsigned int>{42, 43}) return 3;
    FlushDeferredMiniWaveTextureDeletes();
    if (deleted.size() != 2) return 4;
    puts("PASS: actual cache deletion defers both IDs; flush deletes each once");
}
'''
        with tempfile.TemporaryDirectory(prefix='harken-mini-delete-') as directory:
            cpp = Path(directory) / 'lifetime.cpp'
            binary = Path(directory) / 'lifetime'
            cpp.write_text(translation_unit, encoding='utf-8')
            if os.name == 'nt':
                # Use the same installed MSVC toolchain as the product build.
                cmake = Path(directory) / 'CMakeLists.txt'
                cmake.write_text('cmake_minimum_required(VERSION 3.16)\n'
                                 'project(MiniDeleteLifetime LANGUAGES CXX)\n'
                                 'set(CMAKE_CXX_STANDARD 17)\n'
                                 'add_executable(lifetime lifetime.cpp)\n'
                                 'target_include_directories(lifetime PRIVATE "' + ROOT.as_posix() + '")\n',
                                 encoding='utf-8')
                build = Path(directory) / 'build'
                subprocess.run(['cmake', '-S', directory, '-B', str(build), '-G',
                                'Visual Studio 17 2022', '-A', 'x64'], check=True)
                subprocess.run(['cmake', '--build', str(build), '--config', 'Debug'], check=True)
                binary = build / 'Debug/lifetime.exe'
            else:
                subprocess.run([compiler, '-std=c++17', '-I', str(ROOT), str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
