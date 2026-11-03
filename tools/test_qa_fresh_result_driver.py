from pathlib import Path
import tempfile,unittest
from unittest.mock import patch
from qa_fresh_result_driver import FreshResultDriver

class ResultPublicationTests(unittest.TestCase):
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
