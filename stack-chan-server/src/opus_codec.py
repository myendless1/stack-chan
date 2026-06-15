class OpusUnavailableError(RuntimeError):
    pass


class OpusCodec:
    def __init__(self, *, sample_rate: int = 16000, channels: int = 1, frame_duration_ms: int = 60) -> None:
        self.sample_rate = int(sample_rate)
        self.channels = int(channels)
        self.frame_duration_ms = int(frame_duration_ms)
        self.samples_per_frame = self.sample_rate * self.frame_duration_ms // 1000
        try:
            import opuslib  # type: ignore
        except Exception as exc:
            self._opuslib = None
            self._import_error = exc
            self._encoder = None
            self._decoder = None
            return
        self._opuslib = opuslib
        self._import_error = None
        self._encoder = opuslib.Encoder(self.sample_rate, self.channels, opuslib.APPLICATION_VOIP)
        self._decoder = opuslib.Decoder(self.sample_rate, self.channels)

    @property
    def available(self) -> bool:
        return self._opuslib is not None

    def _require(self) -> None:
        if self._opuslib is None:
            raise OpusUnavailableError(
                "opuslib/libopus is not available; install opuslib and libopus for xiaozhi audio frames"
            ) from self._import_error

    def decode(self, opus_frame: bytes) -> bytes:
        self._require()
        if not opus_frame:
            return b""
        return self._decoder.decode(opus_frame, self.samples_per_frame, decode_fec=False)

    def encode(self, pcm_frame: bytes) -> bytes:
        self._require()
        if not pcm_frame:
            return b""
        return self._encoder.encode(pcm_frame, self.samples_per_frame)

    def iter_pcm_frames(self, pcm: bytes):
        bytes_per_frame = self.samples_per_frame * self.channels * 2
        for offset in range(0, len(pcm), bytes_per_frame):
            frame = pcm[offset : offset + bytes_per_frame]
            if len(frame) == bytes_per_frame:
                yield frame
