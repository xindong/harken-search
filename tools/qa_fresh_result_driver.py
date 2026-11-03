"""Keep the result path absent while the app publishes the next atomic response."""
from qa_transport_themes import Driver
import shutil

class FreshResultDriver(Driver):
    def command(self,payload,timeout=25):
        previous=self.runtime/'qa_result.json'
        if previous.exists():
            shutil.move(str(previous),str(self.output/'previous-result.json'))
        return super().command(payload,timeout)
