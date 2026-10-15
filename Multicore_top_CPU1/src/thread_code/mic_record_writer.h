#ifndef MIC_RECORD_WRITER_H
#define MIC_RECORD_WRITER_H

#include <stdbool.h>

void mic_record_writer_poll(bool sd_ready);
void mic_record_capture_poll(void);

#endif /* MIC_RECORD_WRITER_H */
