from pathlib import Path
import errno,subprocess,sys,tempfile,unittest
from unittest.mock import patch
from qa_fresh_result_driver import FreshResultDriver

class ResultPublicationTests(unittest.TestCase):
    def test_cross_volume_previous_result_is_preserved_before_sending(self):
        with tempfile.TemporaryDirectory() as raw:
            root=Path(raw);runtime=root/'runtime';output=root/'output'
            runtime.mkdir();output.mkdir();(runtime/'qa_result.json').write_bytes(b'cross-volume')
            (output/'previous-result.json').write_bytes(b'older')
            def receive(driver,payload,timeout):
                self.assertFalse((runtime/'qa_result.json').exists())
                self.assertEqual((output/'previous-result.json').read_bytes(),b'cross-volume')
                return {'ok':True}
            error=OSError(errno.EXDEV,'Different volumes')
            with patch('os.replace',side_effect=error),patch('os.rename',side_effect=error),patch('qa_transport_themes.Driver.command',receive):
                self.assertTrue(FreshResultDriver(runtime,output).command({'cmd':'settings_ui'})['ok'])

    def test_night_editor_help_does_not_advertise_unimplemented_layout_only(self):
        result=subprocess.run([sys.executable,str(Path(__file__).with_name('qa_settings_night_editor.py')),'--help'],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertNotIn('--layout-only',result.stdout)

    def test_previous_result_is_not_polled_while_next_result_replaces_it(self):
        with tempfile.TemporaryDirectory() as raw:
            root=Path(raw);runtime=root/'runtime';output=root/'output'
            runtime.mkdir();output.mkdir();(runtime/'qa_result.json').write_text('old')
            def receive(driver,payload,timeout):
                self.assertFalse((runtime/'qa_result.json').exists())
                self.assertEqual((output/'previous-result.json').read_text(),'old')
                return {'ok':True}
            with patch('qa_transport_themes.Driver.command',receive):
                self.assertTrue(FreshResultDriver(runtime,output).command({'cmd':'settings_ui'})['ok'])

if __name__=='__main__':unittest.main()
