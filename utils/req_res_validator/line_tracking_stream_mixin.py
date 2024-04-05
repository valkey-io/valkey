import io
import typing


class LineTrackingStringIO(io.StringIO):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._lines_read: int = 0

    def read(self, size: typing.Optional[int] = None) -> str:
        result = super().read(size)
        self._lines_read += result.count("\n")  # Expected to be \r\n but we can also count \n just in case.
        return result

    def readline(self, size: typing.Optional[int] = None) -> str:
        result = super().readline(size)
        if size is None:
            self._lines_read += 1 if result else 0  # Might have been that no data was left, which means no lines read.
        else:
            self._lines_read += result.count("\n")  # Expected to be \r\n but we can also count \n just in case.
        return result

    @property
    def lines_read(self) -> int:
        return self._lines_read
