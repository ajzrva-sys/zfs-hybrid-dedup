import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[3]
SOURCE=ROOT/'cmd/zfs/zfs_chunkdedup_user.c'

def function(s,n):
    lines=s.splitlines(keepends=True)
    for i,l in enumerate(lines):
        if l.startswith(n+'('):
            for j in range(i+1,len(lines)):
                if lines[j]=='}\n':return ''.join(lines[i-1:j+1])
    raise AssertionError(n)

class ValidateOnly(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.folder=Path(cls.tmp.name)
        s=SOURCE.read_text();cls.source=s
        helper=r'''#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
struct cd_checkpoint { int loaded,complete,selected,proof; };
struct cd_catalog { struct cd_checkpoint *checkpoint; uint64_t count; };
struct cd_checkpoint_share_progress { uint64_t generation; };
struct cd_checkpoint_proven_header { uint64_t count; };
static int calls,mode;
static int cd_checkpoint_selected_load(struct cd_checkpoint *c,FILE **s,struct cd_checkpoint_share_progress *p,unsigned char **d){
 (void)c;calls++;if(mode==4)return EIO;*s=tmpfile();*d=malloc(1);p->generation=3;return 0;}
static const struct cd_checkpoint_proven_header *cd_checkpoint_proven_header(struct cd_checkpoint *c){
 static struct cd_checkpoint_proven_header p={9};return c->proof?&p:NULL;}
'''+function(s,'cd_catalog_restore_validate')+r'''
int main(int argc,char **argv){(void)argc;mode=atoi(argv[1]);
 struct cd_checkpoint p={1,1,1,1};struct cd_catalog c={&p,12};
 if(mode==1)p.loaded=0;if(mode==2)p.complete=0;if(mode==3)p.selected=0;if(mode==5)p.proof=0;
 int e=cd_catalog_restore_validate(&c);printf("result=%d calls=%d\n",e,calls);return 0;}
'''
        cls.helper=cls.folder/'helper'; c=cls.folder/'helper.c';c.write_text(helper)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(cls.helper)],check=True,capture_output=True)
        cli=r'''#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <limits.h>
typedef int boolean_t;
#define B_FALSE 0
#define B_TRUE 1
#define MIN(a,b) ((a)<(b)?(a):(b))
enum cd_method {CD_METHOD_AUTO,CD_METHOD_BLOCK,CD_METHOD_CHUNK};
static atomic_int cd_stop;static boolean_t cd_restore_validate_only;
static int cd_watch(uint64_t a,uint64_t b,int c,char **d,enum cd_method e,unsigned f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 2;}
static int cd_pass(const char *a,boolean_t b,uint64_t c,int d,char **e,boolean_t f,enum cd_method g,boolean_t h,unsigned i,const char *j,const char *k){
 (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;(void)k;puts("dispatch-only no-operation");return 0;}
'''+function(s,'cd_signal')+function(s,'zfs_do_chunkdedup')+r'''
int main(int argc,char **argv){return zfs_do_chunkdedup(argc,argv);}
'''
        cls.cli=cls.folder/'cli';c=cls.folder/'cli.c';c.write_text(cli)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(cls.cli)],check=True,capture_output=True)

    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()

    def run_helper(self,mode):
        return subprocess.check_output([str(self.helper),str(mode)],text=True)

    def test_selected_success(self):
        s=self.run_helper(0);self.assertIn('PASS generation=3 files=12 proof_ranges=9',s);self.assertIn('result=0 calls=1',s)

    def test_incomplete_or_unselected_never_load_or_promote(self):
        for mode in (1,2,3):
            s=self.run_helper(mode);self.assertIn('calls=0',s);self.assertNotIn('PASS',s)

    def test_native_loader_failure_propagates(self):
        s=self.run_helper(4);self.assertIn('result=5 calls=1',s);self.assertNotIn('PASS',s)

    def test_missing_proof_refuses(self):
        s=self.run_helper(5);self.assertNotIn('PASS',s);self.assertIn('calls=1',s)

    def test_cli_requires_existing_checkpoint_and_report(self):
        for args in [[],['--report','/report'],['--checkpoint','/missing','--report','/report'],['--checkpoint',str(self.folder)]]:
            r=subprocess.run([str(self.cli),'optimize','--restore-validate-only',*args,'/target'],capture_output=True,text=True)
            self.assertEqual(r.returncode,2);self.assertNotIn('dispatch-only',r.stdout)
        r=subprocess.run([str(self.cli),'optimize','--restore-validate-only','--checkpoint',str(self.folder),'--report','/report','/target'],capture_output=True,text=True)
        self.assertEqual(r.returncode,0);self.assertIn('dispatch-only',r.stdout)

    def test_cli_other_operations_refuse(self):
        for op in ('watch','verify','status','unoptimize'):
            r=subprocess.run([str(self.cli),op,'--restore-validate-only','/target'],capture_output=True,text=True)
            self.assertEqual(r.returncode,2);self.assertNotIn('dispatch-only',r.stdout)

    def test_actual_dispatch_exits_before_scan_and_work(self):
        s=function(self.source,'cd_pass')
        self.assertIn('if (cd_restore_validate_only || error != 0 || catalog.checkpoint->complete)\n\t\t\t\tgoto target_done;',s)
        self.assertIn('if (ret == 0 && !cd_stop && !cd_restore_validate_only)',s)
        helper=function(self.source,'cd_catalog_restore_validate')
        for name in ('cd_catalog_apply','cd_catalog_checkpoint_work','cd_catalog_report_init','cd_checkpoint_selected_save','cd_checkpoint_commit','lzc_chunkdedup'):
            self.assertNotIn(name,helper)

if __name__=='__main__':unittest.main()
