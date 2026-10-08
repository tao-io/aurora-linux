"""Compile actual descriptor construction against native 25G83 PIO samples."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'drivers/spi/spi-apple-qspi.c').read_text()
a=s.index('\tmode = apple_qspi_width_mode(shape.width);', s.index('static int apple_qspi_transfer_op'))
b=s.index('\n\tapple_qspi_p_write(qspi, APPLE_QSPI_P_IRQ_ENABLE',a)
body=s[a:b]
defs='\n'.join(x for x in s.splitlines() if x.startswith('#define APPLE_QSPI_CMD_'))
pre='''#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((~0U << (l)) & (~0U >> (31-(h))))
#define FIELD_PREP(m,v) (((v) << __builtin_ctz(m)) & (m))
#define SPI_MEM_DATA_IN 1
struct op {struct {unsigned dir,nbytes;} data;};
struct shape {unsigned width,shared_count,multi_count,turnaround;};
struct qspi {unsigned tag;};
static unsigned apple_qspi_width_mode(unsigned w) {return w==1?1:w==2?2:3;}
'''
count_line='op->data.dir == SPI_MEM_DATA_IN ? op->data.nbytes : tx_len'
assert count_line in s
post='''
int main(void) {
 /* Native transferData has shared/multi/turnaround zero for TX-only. */
 unsigned lens[]={1,4,5,260}; /* WREN/WRDI, SE, PP1, PP256 */
 for(unsigned i=0;i<4;i++) {
  struct qspi q={.tag=7};struct qspi *qspi=&q;
  struct op o={.data={.dir=0,.nbytes=i<2?0:lens[i]-4}};struct op *op=&o;
  struct shape shape={.width=1,.shared_count=i?4:1};
  unsigned mode,descriptor,tx_len=lens[i];
BODY
  assert(descriptor==0x7031); assert(@COUNT@==lens[i]);
 }
 { struct qspi q={.tag=7};struct qspi *qspi=&q;
   struct op o={.data={.dir=1,.nbytes=16}};struct op *op=&o;
   struct shape shape={.width=1,.shared_count=4};
   unsigned mode,descriptor,tx_len=4;
BODY
   assert(descriptor==0x47039);assert(@COUNT@==16);
 }
 puts("PASS: actual descriptor block matches native TX-only WREN/SE/PP samples; RX split preserved");
}
'''.replace('BODY',body).replace('@COUNT@',count_line)
with tempfile.TemporaryDirectory(prefix='qspi-descriptor-') as d:
 p=Path(d);(p/'test.c').write_text(pre+defs+post)
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
