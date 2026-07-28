# Historical validation observations

This document preserves machine-specific validation and profiling observations
that are useful context for maintainers. They are historical results, not
performance, compatibility, or hardware guarantees.

## Built-in analog-mode exercise

| Field | Recorded information |
| --- | --- |
| Application version, commit, date | Unspecified in the original observation |
| Operating system and processor | Unspecified |
| SDR hardware | RTL-SDR |
| Receiver settings | Unspecified |
| Method | Selected AM, NFM, WFM, USB, and LSB during a stable live spectrum/waterfall run; deterministic tone regressions also covered those modes. |
| Observed result | No intelligible over-air programme content was identified. The five built-in analog modes were verified synthetically; this observation does not claim that any mode was heard over the air. |

WFM output in this observation was mono. Stereo and RDS decoding are not
implemented.

## Mode-specific-decimation profile

| Field | Recorded information |
| --- | --- |
| Application version, commit, date | Unspecified in the original observation |
| Operating system, processor, and SDR hardware | Unspecified |
| Receiver settings | 2 MHz effective hardware rate; 180 kHz WFM for the five-minute run; FFT publication configured near 25 FPS. |
| Method | Compared the original decimation-1 graph with the live GUI using mode-specific decimation. |
| Observed result | The original graph measured about 121–122% CPU in each analog mode. The mode-specific graph measured AM 72%, NFM 75%, default-filter WFM 44%, USB 73%, and LSB 83%. The WFM run produced about 48,000 audio frames per second with no dropped samples or overflows after startup; delivered GUI frames were roughly 15–22 FPS, compared with roughly 23–24 FPS in shorter baseline samples. |

## Long NFM and WFM observation

| Field | Recorded information |
| --- | --- |
| Application version, commit, date | Unspecified; described only as the current release in the original observation |
| Operating system and processor | Unspecified |
| SDR hardware | RTL-SDR |
| Receiver settings | Requested and effective 2.4 MS/s; NFM and WFM each ran for more than five minutes. |
| Method | Observed live spectrum/waterfall updates and audio diagnostics after the 30 ms prefill. |
| Observed result | Neither software nor platform underrun counters increased. WFM retained zero audio overflows. NFM recorded a few bounded upstream scheduler drops but no recurring audio underruns; the device was stopped and re-enumerated afterward. |

## DMR/P25 validation status

No hardware or over-air DMR/P25 decoding validation result is recorded here.
The DMR/P25 path remains an external-process integration whose behavior is
covered by bounded process and audio tests; successful decoding on a particular
DSD-FME installation, signal, or SDR is not claimed by these observations.
