#!/usr/bin/env python3
"""Independent structured gate. PASS strings alone never satisfy a check.

Exit 0=PASS, 1=observed TEST_FAILURE, 2=INFRA_FAILURE (missing execution,
provenance, tampering, malformed evidence, or unavailable environment).
Policy content is frozen in this evaluator; changing policy.json alone cannot
relax acceptance. This is an execution-evidence gate, not remote attestation.
"""
import argparse
import ast
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import tarfile

EXPECTED_POLICY = {'schema': 1,
 'policy_id': 'native-chunkdedup-prototype-v1',
 'base_revision': '71a9f9578616a90c3c14bb59629fb4d31bfd68d1',
 'profiles': ['local', 'native', 'native-smoke'],
 'required_helpers': ['run.py', 'checks.py', 'native.py', 'evaluate.py', 'policy.json', 'repair-schema.json'],
 'local_stages': ['source_freeze',
                  'static_analysis',
                  'baseline_integrity',
                  'toolchain',
                  'chunker_api',
                  'microsoft_conformance',
                  'core_model',
                  'worker_batches',
                  'packaging'],
 'native_stages': ['host_preflight',
                   'media_freeze',
                   'guest_boot',
                   'guest_build',
                   'guest_module_load',
                   'guest_provenance',
                   'guest_regression',
                   'evidence_capture',
                   'guest_restore',
                   'host_restore',
                   'host_state_compare'],
 'assertion_minimums': {'chunker_api': 26, 'core_model': 104, 'worker_batches': 26, 'packaging': 8},
 'toolchain_probes': 2,
 'fixtures': 35,
 'fixture_manifest_sha256': '721f0135eb137d8ca67c1a2a0fa63d3ba9a112e368232a4b1f695049bbcd13cf',
 'oracle_commit': '852a00e5b45ef8a8d06a2eb9040495c36243078f',
 'original_suite_sha256': '83093c772fd82f5bf95e11582b77c883d5fb893ac841e7bba23b2cc70707732a',
 'native_checks': ['guest guard and helper build',
                   'isolated pools and bounded datasets',
                   'deterministic mixed fixtures',
                   'sharing, saved physical space and metadata',
                   'cold native read paths',
                   'foreground logical read and memory metrics',
                   'partial writes, mmap writes and idempotent reoptimization',
                   'rename, hardlink and open-unlinked lifecycle',
                   'truncate, partial EOF append, extension and sparse data',
                   'age, busy file, property, quota and export admission',
                   'tiny files and 40-100 KiB partial EOF records',
                   'atime-on conversion preserves file timestamps and metadata',
                   'sparse extents and bounded physical hole allocation',
                   'background watching and writable mmap races',
                   'short tails crossing 1 MiB conversion batches',
                   'interrupted conversion and resumable mixed mappings',
                   'debug-only collision comparator and corrupt mapping rejection',
                   'snapshot, clone independence and logical copy',
                   'full/incremental replication and receiver feature rejection',
                   'unoptimization, live reference GC and snapshot retention',
                   'quota exhaustion preserves logical reads'],
 'performance': {'minimum_fixture_savings': 0.5,
                 'maximum_paired_read_slowdown': 10.0,
                 'read_samples': 3,
                 'cache_mode': 'remount_without_arc_flush',
                 'method': 'sha256_logical_read',
                 'historical_metrics_can_satisfy_current_run': False,
                 'write_and_memory_numbers': 'context_only'},
 'maximum_repairs': 3,
 'current_native_claim': '21-case regression only; forced-crash and legacy gates require separate fresh '
                         'stages',
 'immutable_baseline': [{'path': 'conformance/README.md',
                         'sha256': 'a85eb749fc52d7b9e6462aba8e6677828b919a9b22cdb1936d29ae8164e225aa'},
                        {'path': 'conformance/SHA256SUMS',
                         'sha256': '5692abede40089167757b5508b4a6eb1c50c7a681a2d1551de52a5c1eab09b2a'},
                        {'path': 'conformance/chunker/LICENSE',
                         'sha256': '88dc396a6da58d834a863fe88a832c268e29d41c3c0419354d4eb7923973c77f'},
                        {'path': 'conformance/chunker/Makefile',
                         'sha256': 'fb051bb9171af6e1674fc49cb3f233827b0f858b15f32e614fcac1c289e40e17'},
                        {'path': 'conformance/chunker/README.md',
                         'sha256': '45c684342945a3841b000ad6fb55b20c697a5c58aac876e54c5f51bb0d2c02c3'},
                        {'path': 'conformance/chunker/build.sh',
                         'sha256': '5f1001c8c69d4b75a3bdb4b699be617bf476d4faa92b5ab386fec459aae64a57'},
                        {'path': 'conformance/chunker/check_fixtures.py',
                         'sha256': '86f0a80b421f405e3a4a103b3f0e1cdbeb49435af136b8e3d1907331057b4cb7'},
                        {'path': 'conformance/chunker/check_oracle_fixtures.py',
                         'sha256': '4e44ec345431d8aaaf94a178cceb40083f38ba78079e9aacbfba4e699e66ea7d'},
                        {'path': 'conformance/chunker/chunkdedup_chunker.c',
                         'sha256': 'a932901b08ed0db1e1f6834fbc9c54097f93a5703fe78fee4cc662981a418946'},
                        {'path': 'conformance/chunker/chunkdedup_chunker.h',
                         'sha256': '35ad9866e076f77b066c34671c058a63853aa54df72dc0bc9d18ac72ed08fe28'},
                        {'path': 'conformance/chunker/chunkdedup_cli.c',
                         'sha256': 'cf6a16e7a560d1516f1a2e9496c7bc43caa5c34a4b47d1e401cc5f88fe70839d'},
                        {'path': 'conformance/chunker/fixture-manifest.json',
                         'sha256': '721f0135eb137d8ca67c1a2a0fa63d3ba9a112e368232a4b1f695049bbcd13cf'},
                        {'path': 'conformance/chunker/fixtures/ascending.bin',
                         'sha256': '84df796bf225d9434d92658e6274c60620eb1390848578adca9bcd0b8c2c7f0b'},
                        {'path': 'conformance/chunker/fixtures/ascending.tsv',
                         'sha256': 'a93f22e447d9848e178b8db02aa66b534aadc55294c457253de137b522d7a0ab'},
                        {'path': 'conformance/chunker/fixtures/empty.bin',
                         'sha256': 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'},
                        {'path': 'conformance/chunker/fixtures/empty.tsv',
                         'sha256': 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'},
                        {'path': 'conformance/chunker/fixtures/ff.bin',
                         'sha256': '4ae03e8ce1675756f52358748187aa658d56e872c3f4d3b893cb3c8f12866398'},
                        {'path': 'conformance/chunker/fixtures/ff.tsv',
                         'sha256': 'd3f3ff3395d8efd792f94f57a653c95b1715f9bfcb35b9a1935c9f8d1323dcb2'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-0.bin',
                         'sha256': 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-0.tsv',
                         'sha256': 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-1.bin',
                         'sha256': '58f7b0780592032e4d8602a3e8690fb2c701b2e1dd546e703445aabd6469734d'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-1.tsv',
                         'sha256': 'ce2c1b75b739563468223e08e263bbea91e12a7b9487b8841a28dcb07e1e49a0'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-1048575.bin',
                         'sha256': '5e68294a2c44fef97c4f96f83da833a63f1266666e74b876c944b003e86a1fad'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-1048575.tsv',
                         'sha256': '5d91c77a0c71e07e3f7401143f0f487d01f454e472aa9feb77c39b543884092d'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-1048576.bin',
                         'sha256': 'bdeb34c21ad1265bc89cf01cd0e4ba34f0e1ab410440a87afe097c222c680da9'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-1048576.tsv',
                         'sha256': 'a6c0f52cdafb740b1ad70422c54c6cd7016004ebd90dc6a6f2a67b3d82b6cb65'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-1048577.bin',
                         'sha256': '24bffbe33499aaa39c6933cb73b9d59854922df226c6ce7d44097c3a31a10741'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-1048577.tsv',
                         'sha256': '08197757b01dd9ac2d0c1d4750f2ca14755468fb8825cd82b9c8bf40b90fc4ab'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-131071.bin',
                         'sha256': '5e5c530e8d57459e09b7645a9c1ce6f492c476c776b0292b010ec0c10949c41d'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-131071.tsv',
                         'sha256': '0d0c2ce7dd9bd809f1b69714216ad9cee5862972bf81f33d2799913860f9d502'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-131072.bin',
                         'sha256': '7b1da5cebe9278a5d20ac46daaede0819224870c1c1da10cad411f436015698d'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-131072.tsv',
                         'sha256': '64101f2b89df888ae0bb09afab6968cf206144565ace5a2ce9da5b0bdea02cce'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-131073.bin',
                         'sha256': '5aaacce33bcd3b9dcbd4732200abc5f32267d5c2f33ae4ab42eae844fffbde0a'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-131073.tsv',
                         'sha256': '35e210cd586a11ebbac058b235d464656e035291ba9db3d64d0ea8831b2f8921'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-2097151.bin',
                         'sha256': '1d54d7f3ff8986b431edeeb07647e0eecf51a51b69509c9a022dbc525cf1f1c5'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-2097151.tsv',
                         'sha256': 'f50891b2b22b6cddc7a5e8d6b48ff93179eb235e6c3917c289237ba8524caf01'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-2097152.bin',
                         'sha256': 'ec8be6f2321624aa2b26fa46cdce822db6ba16e093a728f981dd09d488702fc8'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-2097152.tsv',
                         'sha256': '3599038f8986f44984fe874b4c13108a29dd8ff9ce3473225702a2cd34f488fb'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-2097153.bin',
                         'sha256': 'b4127759c2a6493d87bec82be52761c717c7f27945acd87ca27ade6d9c9ccda1'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-2097153.tsv',
                         'sha256': '407866ad8bc8360b531fa041aca71bf106bca657ddeb2e2c189eeca35c25292c'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-32767.bin',
                         'sha256': '5383a4f33de8a9dcd5c49585893f6de77464a2a2504f3177e66357eac4a5561e'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-32767.tsv',
                         'sha256': '2e45151382370d1a41e07f3ce677212d5e5fde5155bdbf7e378c436517c72730'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-32768.bin',
                         'sha256': 'f2fce862d201f0ab68519dd9e070f1d98f859119827700767cf3663df39cbbb4'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-32768.tsv',
                         'sha256': 'd4bffa671d6ac20b68b0cf3a432f8b8a9adb4f355262645ee1c7a567ab53fcc6'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-32769.bin',
                         'sha256': '59481775d9c6106f54c8cd4d426b2eb0a5e6082b30e9c953f6000584f14c1320'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-32769.tsv',
                         'sha256': 'dc80866b98c6764426062e6b84c534207291064c7276fb1cd0d530e4d884c902'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-524287.bin',
                         'sha256': 'a813dff6dcc8df79bb00aa542c033ab10bae48aa13f623b0fc97a7761b5ddc16'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-524287.tsv',
                         'sha256': '0b9f1f65f856f0ea182d9f42493da20ca0bb426f82945a286dee212f6a36b88e'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-524288.bin',
                         'sha256': '9441c88675deba34e6faf237924413e041a6bd94d97c9aafba4846ded9832f19'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-524288.tsv',
                         'sha256': '3a3807768a6ae53821a75f393f838434f720a2b0b4f6df6764e3ed78a4ae64c0'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-524289.bin',
                         'sha256': '21c2b1de1863f7c0caeaf2a6145f5076092ad4c09b2277ce1c133afb7d35c005'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-524289.tsv',
                         'sha256': 'd8413f611112addfaed31e52ef6a28f4d355817d9a6443e48365853928a9c719'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-65535.bin',
                         'sha256': 'f55b88a6c46ee5dc309ed4b3ab329d61bce494142ae39f6811c606141a7772a2'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-65535.tsv',
                         'sha256': 'b2c6f2e48221ee259596a58088699ab17ea7e99ed5e9ea965d8cc21e805d645b'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-65536.bin',
                         'sha256': 'cfbf303cf5398d46325ba858c336dcd131ffa6c12f5cb8c67857e4193d18fdeb'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-65536.tsv',
                         'sha256': 'de7b5ccb85a4b9d38f8f80daea6ce63dbfd7847d51b7e2c09c5bca292286fb1a'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-65537.bin',
                         'sha256': '3a176de614b220a6cf40d694800cb5d20ba92f51053daa57521ffa7d32999301'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-65537.tsv',
                         'sha256': 'add4795f3cc9e2ddeccdf1bef49ae1cc17aa1a9b5e5e9a572254b67c03aa9710'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-8388608.bin',
                         'sha256': '505052a6c0938243d65e33078cc7b010bc92c9fe2748774fda05dcb16c8776fc'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed0-8388608.tsv',
                         'sha256': '55c2942385810dc1ad8416a1cc9ee152b2f8ac2f819625fb820acfd98df18a59'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed69519-1254972.bin',
                         'sha256': 'e6768b233d074686cf4b975434dca2319063737c1d9eb3fd4935f51ca6d2397b'},
                        {'path': 'conformance/chunker/fixtures/microsoft-seed69519-1254972.tsv',
                         'sha256': 'b2d2f15cc09c11fe75f4d857e115da46a3a2f09497daeda9e74b2feb9bad7355'},
                        {'path': 'conformance/chunker/fixtures/regression-1.bin',
                         'sha256': 'b47ceb1a86a9f43ee0dd7768e8a6012697139291c2f5bce0156e736778545bbc'},
                        {'path': 'conformance/chunker/fixtures/regression-1.cuts.tsv',
                         'sha256': 'c3a0057c93b1fdda5e0b335348f4665c24284ffb292857a18d6cee3d0483adda'},
                        {'path': 'conformance/chunker/fixtures/regression-1.tsv',
                         'sha256': '684b906989efe0ad7a5ad370eec7ca69f0c30508d8bde782631875e90daaae30'},
                        {'path': 'conformance/chunker/fixtures/regression-2.bin',
                         'sha256': '3663304435b3737b3b3f9ca9fcf1c69d0631ec159cfa2a89cd918ffcb59e5c2e'},
                        {'path': 'conformance/chunker/fixtures/regression-2.cuts.tsv',
                         'sha256': '822d0f3831b7e15aad68dbd10f2315d56c8abcc5ec09a664a0b307df20cf647a'},
                        {'path': 'conformance/chunker/fixtures/regression-2.tsv',
                         'sha256': '0e8996640d91800a9ecfb0482b85f1935daa5b156f5875effed13bb6db64bfef'},
                        {'path': 'conformance/chunker/fixtures/regression-3.bin',
                         'sha256': '64e00def81a9c789c182759e9dafdec62b7e4ff8a27e822e26c854f7fd7b86e0'},
                        {'path': 'conformance/chunker/fixtures/regression-3.cuts.tsv',
                         'sha256': 'b61ddc7a6fff05f365f1d5b5e15a04106f0cf430c9d3610bea80ec6c4d02c6e6'},
                        {'path': 'conformance/chunker/fixtures/regression-3.tsv',
                         'sha256': '650bab98151067b80311187b6b92bbf7ac8b4b1e7166ebf3648266bd2ee2e446'},
                        {'path': 'conformance/chunker/fixtures/regression-4.bin',
                         'sha256': 'ff87f0941663c153622f7c9dec3938982ee1a89e1abec7671d38366a21dfb086'},
                        {'path': 'conformance/chunker/fixtures/regression-4.cuts.tsv',
                         'sha256': 'e46b312c967047b3e1439cfb3e77997809e139f4741b893a5c261e1c93d45bc4'},
                        {'path': 'conformance/chunker/fixtures/regression-4.tsv',
                         'sha256': '49fcd90fd1e30baa72e1820b7934988be7c725c915b6e29cba8627d84e9c4e86'},
                        {'path': 'conformance/chunker/fixtures/shifted-seed0-8388608.bin',
                         'sha256': 'fe10252510088fd65391f2465cbdaa17f4a9987764d52a35555b9834134a6101'},
                        {'path': 'conformance/chunker/fixtures/shifted-seed0-8388608.tsv',
                         'sha256': '224167a9e8a0aff8d0e850e823072822b25380dc161536341b5aec198cc3c3c0'},
                        {'path': 'conformance/chunker/fixtures/tiny-nonzero.bin',
                         'sha256': '2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824'},
                        {'path': 'conformance/chunker/fixtures/tiny-nonzero.tsv',
                         'sha256': '10540d5683852f788a37e5245d2d65926c5e2af1c3f38e3d4f3172bd713240e5'},
                        {'path': 'conformance/chunker/fixtures/tiny-zeros.bin',
                         'sha256': '5322fecfc92a5e3248a297a3df3eddfb9bd9049504272e4f572b87fa36d4b3bd'},
                        {'path': 'conformance/chunker/fixtures/tiny-zeros.tsv',
                         'sha256': '7667924b8755acd3f2ea263b28562f89d4cf519a33810135e4878cde10423f2d'},
                        {'path': 'conformance/chunker/fixtures/window-transition.bin',
                         'sha256': '36d900d999dbb06c58851a7565bd819d1aa484565b8010fa1823bf671111f954'},
                        {'path': 'conformance/chunker/fixtures/window-transition.tsv',
                         'sha256': '0dcf5eaa29128fdef18752df040b27eb68579a289ab56b0cda6265c057536d13'},
                        {'path': 'conformance/chunker/fixtures/zero-transition.bin',
                         'sha256': '6fbd7b4a3d3ad41eb04b76e4deb544f32f78af166f24731fbb712b11e5567fd2'},
                        {'path': 'conformance/chunker/fixtures/zero-transition.tsv',
                         'sha256': '1232816b8d36be55a234f2ef346816b70fe06b3d090921cc9439e4cb8a46549b'},
                        {'path': 'conformance/chunker/fixtures/zeros.bin',
                         'sha256': '4ab2d8519974a95ed1555a0568c38944ca5ae5727810a655188b458781e2175b'},
                        {'path': 'conformance/chunker/fixtures/zeros.tsv',
                         'sha256': 'd9c576d117f2377ed1130d0eb369bcceb9135e0524933c4270508b463fd518b7'},
                        {'path': 'conformance/chunker/rabin64_table.h',
                         'sha256': '92c77f01829bd4735d17abfdf0ebaabc221b5c502677d5e58e3c7d2d610f7bb5'},
                        {'path': 'conformance/chunker/test-results/conformance.json',
                         'sha256': '7d5029902041d9956555b0def2906cd8833971a40215acff4ed3efb2bebeb242'},
                        {'path': 'conformance/chunker/test-results/package-verification.json',
                         'sha256': '6e0aa73fe5f1960918b6e6c43a2d2206a01d26e7a95d5e7cf6116e7aa3740822'},
                        {'path': 'conformance/chunker/test-results/sanitizers.txt',
                         'sha256': '76df34cd67a13799a4750a9592e1709d69b7e5e591d07853bc32a91aecf81ecd'},
                        {'path': 'conformance/chunker/test_chunker.c',
                         'sha256': '8c34705ef63a5b9fb99a483ebe864ba24d23c99cc21aad243b06cb2578975753'},
                        {'path': 'conformance/chunker/test_conformance.py',
                         'sha256': 'a15fdade258406bb5cb680e921cd817a228a264137fb26f245dd8de9a63edaf8'},
                        {'path': 'conformance/reference/ChunkInfo.cs',
                         'sha256': 'd595f9b0913988cdd435cb512195d4519a251ed05db79d5d7321fb8976609a47'},
                        {'path': 'conformance/reference/ChunkerConfiguration.cs',
                         'sha256': '54aaeff2e3f8d3d683cd3c93b30b591652b71637d489144efad7e926b6283855'},
                        {'path': 'conformance/reference/DedupBasicChunkInfo.cs',
                         'sha256': '7111dd23cf66de15fc1f8a7a19753af86716a825e1c071671ea6c6757e1cb7c9'},
                        {'path': 'conformance/reference/DedupChunkCutType.cs',
                         'sha256': '4c479b55dacbfd05a3f06053c9ed6ea76c3a69fa0a56a71497ee617b4650315e'},
                        {'path': 'conformance/reference/DedupChunkHashAlgorithm.cs',
                         'sha256': 'b4f201832267c2e6fdd72b15f63d8c888982654028fd74f0dffc9c20540ebeef'},
                        {'path': 'conformance/reference/DedupNode.cs',
                         'sha256': 'fbea1d7a94d95a76eac6a8326581507ff1adc837dfe3db2d1d496bb79dec9144'},
                        {'path': 'conformance/reference/DedupNodeContentHasherTests.cs',
                         'sha256': '77d8e36a123f5a02f261e7b9554c1408bef20e0702f4bd029a0b5bbca06dc07b'},
                        {'path': 'conformance/reference/DeterministicChunker.cs',
                         'sha256': '69809eb1ff864c3a7ecaab67e8d7681dfa88342c566d2e53e9c8194673a0b16d'},
                        {'path': 'conformance/reference/IChunker.cs',
                         'sha256': '5c6d0566eff8e446b651c2accc9e39789ca212d077f3f28829e4c185e386ca2e'},
                        {'path': 'conformance/reference/INonDeterministicChunker.cs',
                         'sha256': '21d06893db083557250596491fd66736f2d33565e27556ae225e8f503c001b4b'},
                        {'path': 'conformance/reference/ManagedChunker.cs',
                         'sha256': '7e3b5d1f2c6de38d16b97ba3f532bdcaaa418910a7daa8a8d4e367b779e262a6'},
                        {'path': 'conformance/reference/Rabin64Table.cs',
                         'sha256': '393ac8b8c55791e5b349733c1257b181928b8aac7ff52a50297244ef5e19007e'},
                        {'path': 'conformance/reference/RegressionChunking.cs',
                         'sha256': '109b0ba5b66145828b05ecf3e3ea17ffbe8fe499ee5612bc9f0f6c7ebd13a7a2'},
                        {'path': 'conformance/reference/dotnet-sdk-provenance.json',
                         'sha256': 'da11857578b03e80e0895c6106c3b06b1b4e456138c1e7310fcee13098e98d31'},
                        {'path': 'conformance/reference/oracle/Oracle.csproj',
                         'sha256': '1bff128dddff40abd550bef5f8c026d6f22861a6045703e0ad5cec4d56799948'},
                        {'path': 'conformance/reference/oracle/Program.cs',
                         'sha256': 'ff2d586d514ab771fa5d515ce7c8c784ddbc36b872b83a6ccc58050369414927'},
                        {'path': 'conformance/reference/oracle/Shims.cs',
                         'sha256': '504693a13db5f92be7a05efb85bbe0bd1ca9b4751b82b07f3553009332cd5e2d'},
                        {'path': 'conformance/reference/oracle/build.sh',
                         'sha256': 'b35074569f06942f77ac7308ac4e756df7e51ed8a84c7e0273914bd8410c1ea1'},
                        {'path': 'conformance/reference/provenance.json',
                         'sha256': '80c629024b64fd6ac9d20b29a91761c654e491c1029d925cfd4feb5f21693fc9'},
                        {'path': 'conformance/verify_bundle.py',
                         'sha256': '7e68d9a1acd0590bf001733d9c9052fe9b1ded09f5ae6a932fbf4e4f366f2c55'},
                        {'path': 'lab-setup/build-media.py',
                         'sha256': '60901007321895c79b71cb81b805a7f570fa2dde3330eb178607cf3db5ab26a4'},
                        {'path': 'lab-setup/guest-build.sh',
                         'sha256': 'a7dc72e2c9bcbdb4480135be12e54dbdec3796b498c27f1e7951078d7be5082e'},
                        {'path': 'lab-setup/guest-report.sh',
                         'sha256': '5eaf40c7ae955ca0b8461ad1c07423cb245544abaf99eda53c4b8cb2bc4a6a9e'},
                        {'path': 'lab-setup/test_media.py',
                         'sha256': '200697b59b2753d9417b28c63f55dbb35e55207cf6b0585d6fe6ae5accc6c14d'},
                        {'path': 'lab-setup/tests/IO_PATHS.md',
                         'sha256': '1991ae33b3711d8e327e64c066e939a9ee52e8511b0fc43e15e531ec360c958d'},
                        {'path': 'lab-setup/tests/REGRESSION.md',
                         'sha256': '98b794610610e4829c40d752b2db27ef4211eab127b1944038ebf45eee13c673'},
                        {'path': 'lab-setup/tests/debug_ctl.c',
                         'sha256': '1b75d6067f05bec43c8a343fd311bf24abc3e54ca108f187d462375bbfa44d1d'},
                        {'path': 'lab-setup/tests/io_paths.c',
                         'sha256': '1fa72a31d8564ebcae72d07cb6700f39131ec228af9140125c2ccadcca15b63b'},
                        {'path': 'lab-setup/tests/legacy_gate.py',
                         'sha256': '3a61412e8ecdaba8f07ee1206477140a3e6ca123aafd23ac5a89f8313b78f179'},
                        {'path': 'lab-setup/tests/regression.py',
                         'sha256': '83093c772fd82f5bf95e11582b77c883d5fb893ac841e7bba23b2cc70707732a'},
                        {'path': 'lab-setup/tests/test_worker_batches.py',
                         'sha256': 'f8297af8aea537a2667f7d83124b1143c5bb4749bdeca2868e007bc0182138b6'},
                        {'path': 'lab-setup/verify-media.sh',
                         'sha256': '1c0b348366b4441da479d0fa6c854a5308a21e0997675a444c454f715eb34cf7'},
                        {'path': 'native-core-mock/README.md',
                         'sha256': '2a7180616a930b61288ff2503e89cf946621acb81951fca18d7780caca44368e'},
                        {'path': 'native-core-mock/RESULTS.md',
                         'sha256': '3795a11a545c1e3d31039a9f83ec6fe965c085d1e9943150d481331e24dad178'},
                        {'path': 'native-core-mock/SHA256SUMS',
                         'sha256': '4e0b7319b90048cba601f812098cc505ea95098b34eebf924b9c3b0a36da5096'},
                        {'path': 'native-core-mock/core_test.c',
                         'sha256': 'b3be2a94f4ddff6c94f9141506590b0fa703f3deaed34835479b9544efd033fe'},
                        {'path': 'native-core-mock/include/sys/dmu.h',
                         'sha256': '8e0b33225a27cda5f46b1a9eec0fa1dedf6f8d3b7eed01547cdd179e8fe5f08f'},
                        {'path': 'native-core-mock/include/sys/dmu_objset.h',
                         'sha256': '8e0b33225a27cda5f46b1a9eec0fa1dedf6f8d3b7eed01547cdd179e8fe5f08f'},
                        {'path': 'native-core-mock/include/sys/dmu_tx.h',
                         'sha256': '8e0b33225a27cda5f46b1a9eec0fa1dedf6f8d3b7eed01547cdd179e8fe5f08f'},
                        {'path': 'native-core-mock/include/sys/dsl_dataset.h',
                         'sha256': '8e0b33225a27cda5f46b1a9eec0fa1dedf6f8d3b7eed01547cdd179e8fe5f08f'},
                        {'path': 'native-core-mock/include/sys/nvpair.h',
                         'sha256': '8e0b33225a27cda5f46b1a9eec0fa1dedf6f8d3b7eed01547cdd179e8fe5f08f'},
                        {'path': 'native-core-mock/include/sys/sa.h',
                         'sha256': '8e0b33225a27cda5f46b1a9eec0fa1dedf6f8d3b7eed01547cdd179e8fe5f08f'},
                        {'path': 'native-core-mock/include/sys/sha2.h',
                         'sha256': '8e0b33225a27cda5f46b1a9eec0fa1dedf6f8d3b7eed01547cdd179e8fe5f08f'},
                        {'path': 'native-core-mock/include/sys/zap.h',
                         'sha256': '8e0b33225a27cda5f46b1a9eec0fa1dedf6f8d3b7eed01547cdd179e8fe5f08f'},
                        {'path': 'native-core-mock/include/sys/zfeature.h',
                         'sha256': '8e0b33225a27cda5f46b1a9eec0fa1dedf6f8d3b7eed01547cdd179e8fe5f08f'},
                        {'path': 'native-core-mock/include/sys/zfs_context.h',
                         'sha256': '5b3f8e203b74d395068661faf094c4f956c00193fecb2660f1e5898c020f1fd9'},
                        {'path': 'native-core-mock/include/sys/zfs_vfsops.h',
                         'sha256': '8e0b33225a27cda5f46b1a9eec0fa1dedf6f8d3b7eed01547cdd179e8fe5f08f'},
                        {'path': 'native-core-mock/include/sys/zfs_znode.h',
                         'sha256': '8e0b33225a27cda5f46b1a9eec0fa1dedf6f8d3b7eed01547cdd179e8fe5f08f'},
                        {'path': 'native-core-mock/run.sh',
                         'sha256': '4b3304c7b485fc84102e10294aabb82a9cd8d50fc54af429b6f23333d7c348a7'}],
 'immutable_baseline_digest': '8a744d0b3a16b8a1694eec6f78b17e71f4bf2979ca535bc49ba5853f8ff7c015'}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def canonical(value):
    return json.dumps(value,sort_keys=True,separators=(',',':'),allow_nan=False)


def pairs(items):
    result={}
    for key,value in items:
        if key in result:
            raise ValueError('duplicate JSON key: '+key)
        result[key]=value
    return result


def read_json(path):
    if Path(path).stat().st_size > 64 << 20:
        raise ValueError('JSON evidence exceeds 64 MiB')
    return json.loads(Path(path).read_text(),object_pairs_hook=pairs,
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))


def integer(value):
    return type(value) is int


def positive(value):
    return type(value) in (int,float) and math.isfinite(value) and value>0


class Gate:
    def __init__(self,run,policy):
        self.root=run.resolve()
        self.policy_path=policy.resolve()
        self.feedback=[]
        self.artifact_count=0
        self.stages={}
        self.documents={}
        self.assessments={}
        self.manifest={}
        self.current='manifest'

    def fail(self,rule,observed,expected,action,category='INFRA_FAILURE'):
        self.feedback.append(dict(category=category,stage=self.current,rule=rule,
                                  observed=observed,expected=expected,action=action))

    def require(self,condition,rule,observed,expected,action,category='INFRA_FAILURE'):
        if not condition:
            self.fail(rule,observed,expected,action,category)
        return condition

    def path(self,base,name):
        if not isinstance(name,str) or not name or Path(name).is_absolute() or '..' in Path(name).parts:
            raise ValueError('unsafe artifact path: '+str(name))
        path=base/name
        if path.is_symlink() or not path.resolve().is_relative_to(self.root):
            raise ValueError('artifact escapes run or is a symlink: '+name)
        return path

    def descriptor(self,base,item):
        if not isinstance(item,dict) or not {'path','sha256'}<=set(item):
            raise ValueError('missing artifact descriptor path/sha256')
        path=self.path(base,item['path'])
        self.require(path.is_file() and sha(path)==item['sha256'],'artifact_integrity',
                     item['path'],'present regular file with matching SHA256',
                     'Preserve logs and regenerate this artifact in a fresh run.')
        self.artifact_count+=1
        return path

    def source(self):
        m=self.manifest
        source=m['source']
        self.require(source.get('base_revision')==EXPECTED_POLICY['base_revision'],
                     'base_revision',source.get('base_revision'),EXPECTED_POLICY['base_revision'],
                     'Use the pinned OpenZFS base and freeze a fresh candidate.')
        root=Path(source['path']).resolve()
        if root!=self.root/'source':
            raise ValueError('source must be the run-local source snapshot')
        files=source['files']
        if not isinstance(files,list) or not files or files!=sorted(files,key=lambda x:x['path']):
            raise ValueError('source manifest must be a nonempty sorted list')
        names=set()
        actual=[]
        for entry in files:
            name=entry['path']
            if name in names or Path(name).is_absolute() or '..' in Path(name).parts:
                raise ValueError('duplicate/unsafe source entry '+name)
            names.add(name)
            path=root/name
            if entry['type']=='symlink':
                if not path.is_symlink():
                    raise ValueError('missing source symlink '+name)
                target=os.readlink(path)
                if os.path.isabs(target) or not path.resolve().is_relative_to(root):
                    raise ValueError('unsafe source symlink '+name)
                got=dict(path=name,type='symlink',sha256=hashlib.sha256(target.encode()).hexdigest(),mode=755)
            elif entry['type']=='file':
                if path.is_symlink() or not path.is_file():
                    raise ValueError('missing regular source '+name)
                got=dict(path=name,type='file',sha256=sha(path),mode=755 if path.stat().st_mode&0o111 else 644)
            else:
                raise ValueError('unknown source type')
            actual.append(got)
        extras={p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file() or p.is_symlink()}-names
        self.require(not extras,'unmanifested_source',sorted(extras),[],
                     'Freeze every compilation input; do not add files after freezing.')
        digest=hashlib.sha256(canonical(actual).encode()).hexdigest()
        self.require(actual==files and digest==source['tree_digest'],'source_integrity',digest,
                     source['tree_digest'],'Discard the changed snapshot and rerun verification.')
        receipt=read_json(self.root/'source-files.json')
        self.require(receipt.get('schema')==1 and receipt.get('files')==files and
                     receipt.get('source_digest')==digest,'source_receipt',receipt.get('source_digest'),digest,
                     'Regenerate the snapshot and source receipt together.')
        self.source_digest=digest
        self.assessments['source_files_verified']=len(actual)

    def controls(self):
        controls=self.manifest['helpers']
        kit=Path(self.manifest['kit']['path']).resolve()
        self.kit=kit
        self.require(set(controls)==set(EXPECTED_POLICY['required_helpers']),
                     'control_completeness',sorted(controls),EXPECTED_POLICY['required_helpers'],
                     'Restore all required runner/evaluator/helper controls.')
        for name,expected in controls.items():
            if Path(name).name!=name:
                raise ValueError('helper names must be plain filenames')
            for path in (self.root/'control'/name,kit/'workflow'/name):
                self.require(path.is_file() and not path.is_symlink() and sha(path)==expected,
                             'control_integrity',str(path),expected,
                             'Stop repair: a test, helper, evaluator or policy changed during this run.')
        self.require(sha(self.policy_path)==self.manifest['policy_sha256']==controls.get('policy.json'),
                     'policy_hash',sha(self.policy_path),controls.get('policy.json'),
                     'Use the unchanged frozen policy; do not weaken thresholds.')
        self.require(sha(__file__)==controls.get('evaluate.py'),'evaluator_identity',sha(__file__),
                     controls.get('evaluate.py'),'Execute the frozen evaluator for this attempt.')

    def stage(self,item):
        name=item['name']
        self.current=name
        if name in self.stages:
            raise ValueError('duplicate stage '+name)
        self.stages[name]=item
        status=item.get('status')
        if status not in ('PASS','TEST_FAILURE','INFRA_FAILURE','SKIPPED'):
            raise ValueError('invalid stage status '+str(status))
        code=item.get('returncode')
        self.require(integer(code),'stage_exit_type',code,'integer exit code','Record the actual subprocess result.')
        log=self.path(self.root,item['log'])
        self.require(log.is_file() and sha(log)==item['log_sha256'],'log_integrity',item['log'],
                     'matching log SHA256','Preserve and recapture the actual process log.')
        if status=='PASS':
            self.require(code==0,'pass_exit_code',code,0,'Do not hide a failing subprocess behind PASS.','TEST_FAILURE')
        elif status=='TEST_FAILURE':
            self.fail('executed_test_failed',{'returncode':code,'log':item['log']},
                      'all required assertions pass','Diagnose the observed log and repair only implementation source.',
                      'TEST_FAILURE')
        elif status=='INFRA_FAILURE':
            self.fail('stage_infrastructure',{'returncode':code,'log':item['log']},
                      'stage executed with complete evidence','Fix the unavailable environment; do not treat this as a source pass.')
        else:
            blocked=item.get('evidence',{}).get('blocked_by')
            failed={x['name'] for x in self.manifest['stages'] if x.get('status') in ('TEST_FAILURE','INFRA_FAILURE')}
            self.require((self.manifest['profile']=='native-smoke' and name=='guest_build') or
                         (name in EXPECTED_POLICY['native_stages'] and blocked in failed),
                         'unauthorized_skip',{'stage':name,'blocked_by':blocked},
                         'explicit prerequisite failure or provenance-matched smoke build',
                         'Execute required stages; a standalone skip cannot satisfy native coverage.')
        self.require(type(item.get('seconds')) in (int,float) and math.isfinite(item['seconds']) and item['seconds']>=0,
                     'stage_duration',item.get('seconds'),'finite nonnegative seconds','Record real elapsed time.')
        evidence=item.get('evidence',{})
        if not isinstance(evidence,dict):
            raise ValueError('stage evidence must be an object')
        base=self.path(self.root,item.get('evidence_root','.'))
        docs=[]
        for desc in evidence.get('artifacts',[]):
            path=self.descriptor(base,desc)
            if path.suffix=='.json' and path.is_file():
                docs.append((path,read_json(path)))
        self.documents[name]=docs

    def helpers(self,folder,names):
        path=self.root/folder/'results.json'
        if not path.exists():
            raise ValueError('missing structured helper report '+str(path))
        data=read_json(path)
        self.require(data.get('schema')==1 and data.get('run_id')==self.manifest['run_id'] and
                     data.get('source_digest')==self.source_digest,'helper_run_binding',
                     {k:data.get(k) for k in ('schema','run_id','source_digest')},
                     {'schema':1,'run_id':self.manifest['run_id'],'source_digest':self.source_digest},
                     'Rerun the helper against this frozen snapshot; historical reports cannot substitute.')
        rows=data.get('results',[])
        self.require(isinstance(rows,list) and len(rows)==len(names) and
                     set(x['name'] for x in rows)==set(names),'helper_stage_coverage',
                     [x.get('name') for x in rows],names,'Restore all required checks; do not remove failing tests.','TEST_FAILURE')
        for row in rows:
            top=self.stages.get(row['name'])
            self.require(top is not None and all(top.get(k)==row.get(k) for k in
                         ('status','returncode','seconds','command','log_sha256','evidence')),
                         'helper_result_consistency',row['name'],'identical helper and orchestrator result',
                         'Do not replace, relabel or suppress helper failures.')
        tested=data.get('tested_sources_digest')
        tested_files=None
        if folder=='local':
            receipt=self.descriptor(self.root/folder,data['tested_sources_manifest'])
            tested_doc=read_json(receipt)
            tested_files=tested_doc.get('files')
            self.require(tested_doc.get('schema')==1 and tested_doc.get('tested_sources_digest')==tested,
                         'tested_source_receipt',tested_doc.get('tested_sources_digest'),tested,
                         'Capture the tested-source inventory and its independent digest.')
            required={'cmd/zfs/chunkdedup_chunker.c','cmd/zfs/chunkdedup_chunker.h',
                      'cmd/zfs/rabin64_table.h','cmd/zfs/zfs_chunkdedup_user.c',
                      'include/sys/zfs_chunkdedup.h','module/zfs/zfs_chunkdedup.c',
                      'cmd/zfs/chunkdedup_hybrid.c','cmd/zfs/chunkdedup_hybrid.h',
                      'cmd/zfs/chunkdedup_parallel.h',
                      'include/sys/dsl_pool.h','module/zfs/dsl_pool.c'}
            self.require(isinstance(tested_files,list) and {x['path'] for x in tested_files}==required,
                         'tested_source_coverage',tested_files,sorted(required),
                         'Include all actual chunker, worker, native-core and dirty-data gauge implementation inputs.')
        if tested_files is not None:
            source_map={x['path']:x for x in self.manifest['source']['files']}
            self.require(isinstance(tested_files,list) and tested_files and all(x==source_map.get(x['path']) for x in tested_files),
                         'tested_source_entries',tested_files,'entries matching frozen source',
                         'Compile only the current frozen implementation.')
            self.require(tested==hashlib.sha256(canonical(tested_files).encode()).hexdigest(),
                         'tested_source_digest',tested,'digest of independently enumerated tested entries',
                         'Record the tested sources and digest from the compile inputs.')
        elif folder=='local':
            self.require(tested==self.source_digest,'tested_source_digest',tested,self.source_digest,
                         'Record an independent full snapshot inventory or tested-source entry list.')
        return data

    def baseline(self):
        self.current='baseline_integrity'
        entries=EXPECTED_POLICY['immutable_baseline']
        for entry in entries:
            path=self.kit/entry['path']
            self.require(path.is_file() and not path.is_symlink() and sha(path)==entry['sha256'],
                         'immutable_baseline',entry['path'],entry['sha256'],
                         'Restore the established fixture/harness. Refreshing kit checksums cannot approve changed tests.')
        item=self.stages.get(self.current)
        if not item or item['status']!='PASS':
            return
        receipts=[d for p,d in self.documents[self.current] if p.name=='baseline-files.json']
        if len(receipts)!=1 or not isinstance(receipts[0],list):
            raise ValueError('missing unique baseline-files.json receipt')
        wanted=[e for e in entries if e['path'].startswith(('conformance/','native-core-mock/')) or
                e['path'] in ('lab-setup/build-media.py','lab-setup/test_media.py',
                              'lab-setup/verify-media.sh','lab-setup/tests/test_worker_batches.py')]
        got=sorted(receipts[0],key=lambda x:x['path'])
        self.require(got==wanted,'baseline_receipt',len(got),len(wanted),
                     'Run all pinned tests; do not replace or omit established harnesses.')
        self.require(item['evidence'].get('counts',{}).get('verified_files')==len(wanted),
                     'baseline_exact_count',item['evidence'].get('counts'),len(wanted),
                     'Report the exact independently verified baseline count.')
        for entry in wanted:
            self.descriptor(self.root/'local'/'baseline',entry)
        self.assessments['immutable_baseline_files_verified']=len(entries)

    def local(self):
        names=EXPECTED_POLICY['local_stages'][2:]
        self.helpers('local',names)
        self.baseline()
        for name in names:
            item=self.stages.get(name)
            if not item or item['status']!='PASS':
                continue
            self.current=name
            e=item['evidence']
            if name=='baseline_integrity':
                counts=e.get('counts',{})
                self.require(counts.get('fixture_pairs')==35 and counts.get('mismatches')==0 and
                             integer(counts.get('verified_files')) and counts['verified_files']>70,
                             'baseline_counts',counts,'35 pairs, >70 verified immutable files, zero mismatches',
                             'Restore the pinned fixtures and controls.','TEST_FAILURE')
            elif name=='toolchain':
                c=e.get('counts',{})
                log=self.path(self.root,item['log']).read_text(errors='replace')
                self.require(integer(e.get('negative_probe_returncode')) and e['negative_probe_returncode']!=0 and
                             'AddressSanitizer: heap-buffer-overflow' in log,
                             'instrumentation_runtime',e.get('negative_probe_returncode'),
                             'a failing runtime access diagnosed by AddressSanitizer',
                             'Enable real instrumentation; compile flags alone are insufficient.')
                self.require(c.get('probes_expected')==2 and c.get('probes_completed')==2,
                             'toolchain_probes',c,'positive sanitized runtime + detected bad access',
                             'Use a working ASan/UBSan compiler; flags alone do not prove instrumentation.')
            elif name in EXPECTED_POLICY['assertion_minimums']:
                a=e.get('assertions',{})
                minimum=EXPECTED_POLICY['assertion_minimums'][name]
                self.require(a.get('expected_minimum')==minimum and integer(a.get('observed')) and
                             a['observed']>=minimum and a.get('failed')==0,
                             'assertion_coverage',a,{'minimum':minimum,'failed':0},
                             'Execute the complete immutable harness with assertions enabled.','TEST_FAILURE')
                proof=[d for _,d in self.documents[name] if isinstance(d,dict) and
                       d.get('assertions')==a.get('observed') and d.get('failed')==a.get('failed')]
                self.require(bool(proof),'assertion_artifact',a,'hashed runtime assertion receipt',
                             'Capture the runtime counter output, not only a summary claim.')
        item=self.stages.get('worker_batches')
        if item and item['status']=='PASS':
            self.current='worker_batches'
            policy=[d for p,d in self.documents['worker_batches'] if p.name=='hybrid-policy.json']
            self.require(len(policy)==1 and policy[0].get('status')=='PASS' and
                         policy[0].get('cases')==13 and policy[0].get('threshold_percent')==20 and
                         policy[0].get('overflow_safe') is True and policy[0].get('uncertain_favors_native') is True and
                         policy[0].get('header_sha256')==sha(self.root/'source/cmd/zfs/chunkdedup_hybrid.h') and
                         policy[0].get('test_sha256')==sha(self.kit/'lab-setup/tests/test_hybrid_policy.py'),
                         'hybrid_policy_boundary',policy,'current header; exact boundary, overflow and uncertainty checks',
                         'Run the focused current-source hybrid selection test.','TEST_FAILURE')
        self.conformance()

    def conformance(self):
        self.current='microsoft_conformance'
        item=self.stages.get(self.current)
        if not item or item['status']!='PASS':
            return
        manifest_path=self.kit/'conformance/chunker/fixture-manifest.json'
        if sha(manifest_path)!=EXPECTED_POLICY['fixture_manifest_sha256']:
            raise ValueError('pinned fixture manifest changed')
        reference=read_json(manifest_path)
        self.require(reference.get('count')==35 and reference.get('oracle_commit')==EXPECTED_POLICY['oracle_commit'],
                     'oracle_fixture_pin',reference.get('oracle_commit'),EXPECTED_POLICY['oracle_commit'],
                     'Restore the pinned Microsoft oracle fixtures.')
        expected={x['input']:x for x in reference['fixtures']}
        documents=[(p,d) for p,d in self.documents[self.current] if p.name=='fixture-results.json']
        if len(documents)!=1:
            raise ValueError('missing unique fixture-results.json evidence')
        path,data=documents[0]
        rows=data.get('fixtures',data.get('results')) if isinstance(data,dict) else data
        if not isinstance(rows,list):
            raise ValueError('fixture results must be a list')
        self.require(len(rows)==35 and len({x['name'] for x in rows})==35 and
                     set(x['name'] for x in rows)==set(expected),'fixture_coverage',
                     [x.get('name') for x in rows],sorted(expected),'Run all 35 pinned fixtures.','TEST_FAILURE')
        total=0
        for row in rows:
            fixture=expected.get(row['name'])
            if fixture is None:
                continue
            out=self.descriptor(self.root/'local',row['output'])
            actual=out.read_bytes()
            oracle=self.kit/'conformance/chunker/fixtures'/fixture['expected']
            source=self.kit/'conformance/chunker/fixtures'/fixture['input']
            count=len(actual.splitlines())
            good=(sha(source)==fixture['sha256'] and sha(oracle)==fixture['expected_sha256'] and
                  actual==oracle.read_bytes() and count==fixture['chunks'] and
                  row.get('input_sha256')==fixture['sha256'] and
                  row.get('expected_sha256')==fixture['expected_sha256'] and
                  row.get('actual_sha256')==sha(out) and row.get('expected_chunks')==fixture['chunks'] and
                  row.get('actual_chunks')==count and row.get('boundaries_match') is True and row.get('hashes_match') is True)
            self.require(good,'fixture_boundary_hash',row['name'],'exact pinned offsets/lengths/hash TSV bytes',
                         'Compare the actual chunk boundaries/hashes; repair the chunker implementation.','TEST_FAILURE')
            total+=count
        c=item['evidence'].get('counts',{})
        self.require(c.get('fixtures_expected')==35 and c.get('fixtures_completed')==35 and
                     c.get('chunks_expected')==sum(x['chunks'] for x in expected.values()) and
                     c.get('chunks_verified')==total and c.get('mismatches')==0,
                     'conformance_counters',c,'35 complete fixtures with independently counted equal chunks',
                     'Do not summarize partial or differing outputs as conformance.','TEST_FAILURE')
        self.assessments['conformance_fixtures_verified']=len(rows)

    def document(self,name):
        found={p:d for docs in self.documents.values() for p,d in docs if p.name==name}
        if len(found)!=1:
            raise ValueError('missing unique hashed artifact '+name)
        return next(iter(found.values()))

    def native_proof(self,report):
        self.current='guest_provenance'
        config=self.document('config.json')
        run_id=self.manifest['run_id'];nonce=report.get('payload_nonce')
        self.require(config.get('run_id')==run_id and config.get('nonce')==nonce and
                     isinstance(nonce,str) and len(nonce)==32 and
                     config.get('source_digest')==self.source_digest,
                     'media_run_binding',config,'current source, run and payload nonce',
                     'Recreate and boot the verified read-only inputs for this attempt.')
        for name in ('source-before.json','source-after.json'):
            proof=self.document(name)
            self.require(proof.get('source_digest')==self.source_digest and
                         proof.get('files')==self.manifest['source']['files'],
                         'guest_source_inventory',name,'all frozen entries identical before/after build',
                         'Discard a changed guest source/build and rebuild this exact snapshot.')
        loaded=self.document('loaded.json');build=self.document('build-status.json')
        guest=self.document('guest-provenance.json')
        self.require(loaded.get('run_id')==run_id and loaded.get('payload_nonce')==nonce and
                     loaded.get('source_digest')==build.get('source_digest')==guest.get('source_digest')==self.source_digest and
                     guest.get('run_id')==run_id and guest.get('payload_nonce')==nonce,
                     'loaded_build_binding',{'loaded':loaded,'guest':guest},
                     'current run/source build and loaded module receipt',
                     'Load the fresh binary/module and recapture their provenance.')
        for key in ('module_sha256','binary_sha256'):
            value=loaded.get(key)
            self.require(isinstance(value,str) and len(value)==64 and
                         value==build.get(key)==guest.get(key),
                         'installed_digest_consistency',key,'equal build, loaded and guest identities',
                         'Verify the staged install and loaded module against the fresh build.')
        self.require(str(loaded.get('module_version','')).startswith('2.4.4-') and
                     guest.get('cpu')==1 and guest.get('interfaces')==['lo0'] and
                     str(guest.get('release','')).startswith('15.1-') and
                     guest.get('prefix')==config.get('prefix'),
                     'guest_environment',guest,'FreeBSD15.1, one CPU, lo0 only, this run prefix',
                     'Restore the approved lab configuration; do not compare a different environment.')
        if self.manifest['profile']=='native':
            self.require(config.get('mode')=='build' and build.get('build_run_id')==run_id and
                         positive(build.get('started')) and positive(build.get('finished')) and
                         build['finished']>=build['started'],
                         'fresh_build',build,'full build from this current run',
                         'Build current sources; old installed binaries cannot verify a new candidate.')
        native=self.document('native-result.json')
        self.require(native.get('payload_nonce')==nonce and native.get('guest',{}).get('module_sha256')==loaded.get('module_sha256') and
                     native.get('guest',{}).get('binary_sha256')==loaded.get('binary_sha256') and
                     positive(native.get('started')) and positive(native.get('finished')) and native['finished']>=native['started'] and
                     native.get('recovery')=='not_run' and native.get('legacy')=='not_run',
                     'fresh_native_execution',native.get('started'),'nonce-bound current binary/module and execution interval',
                     'Run the current guest suite and report fresh crash/legacy evidence separately.')
        started=datetime.datetime.fromisoformat(self.manifest['started']).timestamp()
        now=datetime.datetime.now(datetime.timezone.utc).timestamp()
        self.require(native.get('started',0)>=started-300 and native.get('finished',now+1)<=now+300,
                     'execution_freshness',{'started':native.get('started'),'finished':native.get('finished')},
                     'timestamps within this workflow interval (300-second clock tolerance)',
                     'Synchronize lab time and rerun; archived evidence cannot satisfy current execution.')
        self.native_media(config)
        self.current='host_state_compare'
        before=self.document('host-before.json');after=self.document('host-after.json')
        comparison=self.document('host-comparison.json')
        self.require(before.get('static')==after.get('static') and before.get('lab_config')==after.get('lab_config') and
                     before.get('lab_state')==after.get('lab_state')==['Stopped'] and
                     comparison.get('passed') is True and comparison.get('lab_stopped') is True and
                     comparison.get('vm_config_unchanged') is True and
                     comparison.get('static_before')==before.get('static') and comparison.get('static_after')==after.get('static'),
                     'host_preservation',comparison,'identical independently hashed before/after static configuration and service state',
                     'Stop the lab and restore the observed changed host/VM state.')
        for label,doc in (('before',before),('after',after)):
            self.require(type(doc.get('used')) is int and doc['used']<48<<30 and
                         type(doc.get('free')) is int and doc['free']>=128<<30,
                         'host_storage_bounds',{'time':label,'used':doc.get('used'),'free':doc.get('free')},
                         'lab usage below48GiB, host available at least128GiB',
                         'Stop writes and restore storage headroom.')
        self.current='guest_restore'
        baseline=self.document('baseline.json');restore=self.document('restore.json')
        self.require(restore.get('baseline_module_sha256')==baseline.get('sha256') and
                     restore.get('loader_unchanged') is True and restore.get('rc_unchanged') is True and
                     restore.get('pools_exported') is True,
                     'guest_restoration',restore,'original module/config and only test pools exported',
                     'Restore the lab guest before accepting this attempt.')
        self.assessments['native_provenance']={'source_entries':len(self.manifest['source']['files']),
            'fresh_build':self.manifest['profile']=='native','module_sha256':loaded.get('module_sha256'),
            'binary_sha256':loaded.get('binary_sha256'),'host_restored':True}

    def native_media(self,config):
        self.current='media_freeze'
        media=self.root/'native/media'
        receipt=self.document('SHA256.json')
        required={'config.json','source-files.json','source.tar.gz','tests.tar.gz','native_driver.py'}
        if 'hybrid_tests' in config:
            required.add('hybrid.tar.gz')
        self.require(set(receipt)==required,'media_coverage',sorted(receipt),sorted(required),
                     'Freeze source, adapters and guest driver as one immutable media input.')
        for name,value in receipt.items():
            self.descriptor(media,dict(path=name,sha256=value))
        iso=self.root/'native/source.iso'
        self.require(iso.is_file() and not iso.is_symlink() and sha(iso)==self.stages[self.current]['evidence'].get('iso_sha256'),
                     'source_iso_integrity',str(iso),'verified input ISO SHA256',
                     'Preserve the uploaded read-only image for independent verification.')
        manifest=self.document('source-files.json')
        self.require(manifest.get('files')==self.manifest['source']['files'] and
                     manifest.get('source_digest')==self.source_digest,
                     'media_source_receipt',manifest.get('source_digest'),self.source_digest,
                     'Create the input media from the current frozen source inventory.')
        expected={x['path']:x for x in self.manifest['source']['files']}
        with tarfile.open(media/'source.tar.gz','r:gz') as archive:
            members=archive.getmembers()
            self.require(len(members)==len(expected),'media_source_coverage',len(members),len(expected),
                         'Package every source entry exactly once.')
            seen=set()
            for member in members:
                if not member.name.startswith('source/'):
                    raise ValueError('invalid source archive member '+member.name)
                name=member.name[7:];entry=expected.get(name)
                if entry is None or name in seen or member.size>64<<20:
                    raise ValueError('unexpected/duplicate/oversized source archive member '+name)
                seen.add(name)
                digest=hashlib.sha256(member.linkname.encode()).hexdigest() if member.issym() else                        hashlib.sha256(archive.extractfile(member).read()).hexdigest() if member.isfile() else None
                self.require(digest==entry['sha256'] and
                             (member.issym() if entry['type']=='symlink' else member.isfile()) and
                             (755 if member.mode&0o111 else 644)==entry['mode'],
                             'media_source_bytes',name,entry,'Package exact current source bytes and modes.')
        adapters=config.get('adapter',{})
        self.require(set(adapters)=={'regression.py','io_paths.c','debug_ctl.c'},
                     'adapter_coverage',list(adapters),'only three pinned original test files',
                     'Adapt destinations only; preserve all original assertions.')
        replacements={
            'regression.py':[('cdlab',config['pool'],64),('/opt/chunkdedup',config['prefix'],6),
                             ("'chunkdedup', 'optimize'", "'chunkdedup', 'optimize', '--method=chunk'",7),
                             ("'chunkdedup', 'watch'", "'chunkdedup', 'watch', '--method=chunk'",2)],
            'io_paths.c':[('/mnt/cdlab/','/mnt/'+config['pool']+'/',3)],
            'debug_ctl.c':[('strncmp(dataset, "cdlab/", 6)',
                            'strncmp(dataset, "'+config['pool']+'/", '+str(len(config['pool'])+1)+')',1),
                           ('strncmp(resolved, "/mnt/cdlab/", 11)',
                            'strncmp(resolved, "/mnt/'+config['pool']+'/", '+str(len('/mnt/'+config['pool']+'/'))+')',1),
                           ('/root/lab/tests/work',config['guest_run']+'/tests/work',2)]}
        with tarfile.open(media/'tests.tar.gz','r:gz') as archive:
            members=archive.getmembers()
            if len(members)!=3 or {x.name for x in members}!={'tests/'+x for x in adapters}:
                raise ValueError('test archive omitted or added harnesses')
            for name,changes in replacements.items():
                original=(self.kit/'lab-setup/tests'/name).read_text();text=original
                substitutions=[]
                for old,new,count in changes:
                    if text.count(old)!=count:
                        raise ValueError('original adapter literal changed '+name)
                    text=text.replace(old,new);substitutions.append(dict(**{'from':old,'to':new,'count':count}))
                record=adapters[name]
                self.require(record=={'original_sha256':hashlib.sha256(original.encode()).hexdigest(),
                             'adapted_sha256':hashlib.sha256(text.encode()).hexdigest(),'substitutions':substitutions} and
                             archive.extractfile('tests/'+name).read()==text.encode(),
                             'adapter_bytes',name,'only pinned literal substitutions in immutable original tests',
                             'Restore the original suite; changing tests cannot repair implementation failures.')
        hybrid_tests=config.get('hybrid_tests',{})
        self.require(set(hybrid_tests)=={'hybrid.py','hybrid_ctl.c'},
                     'hybrid_test_inventory',hybrid_tests,'two separately frozen hybrid test files',
                     'Freeze the real CLI suite and bounded native caller separately from the legacy harness.')
        with tarfile.open(media/'hybrid.tar.gz','r:gz') as archive:
            members=archive.getmembers()
            self.require(len(members)==2 and {m.name for m in members}=={'hybrid/'+n for n in hybrid_tests},
                         'hybrid_archive_coverage',[m.name for m in members],sorted(hybrid_tests),
                         'Package exactly the separately declared hybrid tests.')
            for member in members:
                name=Path(member.name).name
                self.require(member.isfile() and member.size<=1<<20 and name in hybrid_tests and
                             hashlib.sha256(archive.extractfile(member).read()).hexdigest()==hybrid_tests[name] and
                             sha(self.kit/'lab-setup/tests'/name)==hybrid_tests[name],
                             'hybrid_test_bytes',name,hybrid_tests.get(name),
                             'Use the frozen current hybrid test bytes without archive substitutions.')
        original=ast.parse((self.kit/'lab-setup/tests/regression.py').read_text())
        main=next(x for x in original.body if isinstance(x,ast.If) and '__name__' in ast.unparse(x.test))
        cases=[]
        for item in main.body:
            if isinstance(item,ast.Expr) and isinstance(item.value,ast.Call) and                     isinstance(item.value.func,ast.Name) and item.value.func.id=='check':
                cases.append([ast.literal_eval(item.value.args[0]),item.value.args[1].id])
        self.require(config.get('cases')==cases and [x[0] for x in cases]==EXPECTED_POLICY['native_checks'],
                     'native_suite_dispatch',config.get('cases'),cases,'Execute all original test functions in the established order.')
        self.require(config.get('pool')=='cdwf-'+self.manifest['run_id'] and
                     config.get('guest_run')=='/root/lab/workflow/'+self.manifest['run_id'],
                     'isolated_run_destinations',config.get('pool'),'unique current-run file-vdev pool and working directory',
                     'Use fresh isolated run destinations and retain existing disk pools.')
        tree=ast.parse((self.root/'control/native.py').read_text())
        driver=next(ast.literal_eval(x.value) for x in tree.body if isinstance(x,ast.Assign) and
                    any(isinstance(t,ast.Name) and t.id=='GUEST_DRIVER' for t in x.targets))
        self.require((media/'native_driver.py').read_text()==driver,'guest_driver_identity',
                     'media/native_driver.py','frozen runner embedded driver',
                     'Freeze the guest driver with the unchanged runner controls.')

    def native(self):
        report=self.helpers('native',EXPECTED_POLICY['native_stages'])
        self.current='guest_regression'
        item=self.stages.get(self.current)
        if not item or item['status']!='PASS':
            return
        if any(self.stages.get(name,{}).get('status') not in ('PASS','SKIPPED') for name in EXPECTED_POLICY['native_stages']):
            return
        self.native_proof(report)
        self.current='guest_regression'
        candidates=[]
        for name,docs in self.documents.items():
            for path,data in docs:
                if path.name=='native-result.json':
                    if path not in {p for p,_ in candidates}:
                        candidates.append((path,data))
        if len(candidates)!=1:
            raise ValueError('missing unique current native-result.json')
        _,data=candidates[0]
        self.require(data.get('run_id')==self.manifest['run_id'] and
                     data.get('source_digest')==self.source_digest and
                     data.get('original_suite_sha256')==EXPECTED_POLICY['original_suite_sha256'],
                     'native_provenance', {k:data.get(k) for k in ('run_id','source_digest','original_suite_sha256')},
                     'current nonce/source and pinned original21-case suite','Rebuild/rerun the isolated guest; do not use archived pass reports.')
        checks=data.get('results',data.get('checks'))
        if not isinstance(checks,list):
            raise ValueError('missing native structured checks')
        by_name={x.get('test',x.get('name')):x for x in checks}
        self.require(len(checks)==21 and len(by_name)==21 and set(by_name)==set(EXPECTED_POLICY['native_checks']),
                     'native_coverage',list(by_name),EXPECTED_POLICY['native_checks'],
                     'Run the complete 21-case current guest suite.','TEST_FAILURE')
        for name,row in by_name.items():
            self.require(row.get('passed') is True,'native_assertion',{'test':name,'error':row.get('error')},
                         True,'Inspect the current guest failing check and repair its implementation.','TEST_FAILURE')
        hybrid=[(p,d) for docs in self.documents.values() for p,d in docs if p.name=='hybrid-result.json']
        hybrid=list({str(p):(p,d) for p,d in hybrid}.values())
        self.require(len(hybrid)==1,'hybrid_native_result',len(hybrid),1,
                     'Run the separately bound current hybrid suite.','TEST_FAILURE')
        if len(hybrid)==1:
            path,result=hybrid[0]
            wanted=(
                'hybrid CLI contracts and forced methods',
                'native exact sharing preserves metadata and COW',
                'automatic unique data stays native',
                'automatic shifted cohort selects chunking',
                'native sparse tails and incompatible geometry',
                'verified sharing rejects stale unequal and busy ranges',
                'native peers and snapshots survive CDC conversion',
                'existing CDC resume and explicit restoration',
                'automatic background watching shares native blocks',
                'interrupted CDC conversion resumes through automatic mode',
                'exhausted quota preserves content during conversion',
                'hybrid snapshots clones and logical replication')
            hybrid_rows=result.get('checks',[])
            self.require(result.get('run_id')==self.manifest['run_id'] and
                         result.get('payload_nonce')==data.get('payload_nonce') and
                         result.get('source_digest')==self.source_digest and result.get('status')=='PASS' and
                         len(hybrid_rows)==len(wanted) and tuple(x.get('test') for x in hybrid_rows)==wanted and
                         all(x.get('passed') is True for x in hybrid_rows),
                         'hybrid_native_coverage',result,'all twelve separate current-source hybrid cases pass',
                         'Inspect the hybrid case failure; do not change the legacy coverage.','TEST_FAILURE')
            binding=data.get('hybrid_result',{})
            self.require(binding.get('path')==path.name and binding.get('sha256')==sha(path) and
                         binding.get('test_files')==result.get('test_files')==
                         read_json(self.root/'native/media/config.json').get('hybrid_tests'),
                         'hybrid_result_binding',binding,'nonce/source/test-inventory and exact result hash',
                         'Bind fresh hybrid evidence to the same run and frozen media.')
        def detail(name):
            return by_name.get(name,{}).get('detail',{})
        sharing=detail('sharing, saved physical space and metadata')
        space=sharing.get('physical_dataset_used_bytes',{})
        before,after=space.get('before'),space.get('after')
        savings=1-after/before if positive(before) and positive(after) else None
        self.require(savings is not None and savings>=0.5,'physical_savings',savings,{'minimum':0.5},
                     'Investigate actual dataset allocation; logical counters do not establish savings.','TEST_FAILURE')
        stats=sharing.get('stats',{})
        expected_bytes=detail('deterministic mixed fixtures').get('total_live_fixture_bytes')
        self.require(stats.get('files')==7 and stats.get('logical_bytes')==expected_bytes and
                     positive(stats.get('chunks')) and stats.get('references',0)>stats['chunks']*3,
                     'sharing_content_counts',stats,'7 files, exact fixture logical bytes, shared chunk references',
                     'Verify mappings and all source bytes; do not accept missing tails.','TEST_FAILURE')
        sparse=detail('sparse extents and bounded physical hole allocation')
        self.require(sparse.get('data_extents_before')==[[0,131072],[4063232,4194304]] and
                     sparse.get('data_extents_after')==sparse.get('data_extents_before') and
                     sparse.get('native_records_with_recipes')==2,'sparse_extents',sparse,
                     'unchanged exact native data extents,2 recipes','Preserve genuine native holes.','TEST_FAILURE')
        tail=detail('short tails crossing 1 MiB conversion batches')
        self.require(tail.get('sizes')==[1048699,16777339] and tail.get('all_tail_records_mapped') is True and
                     tail.get('idempotent_reoptimization') is True,'tail_coverage',tail,'both EOF tails mapped and idempotent',
                     'Flush retained boundary chunks and retest interruption/resume.','TEST_FAILURE')
        gc=detail('unoptimization, live reference GC and snapshot retention')
        self.require(all(gc.get('stats',{}).get(k)==0 for k in ('files','records','chunks','chunk_bytes','logical_bytes','references')) and
                     positive(gc.get('snapshot_retained_physical_bytes')),'live_gc_snapshot',gc,
                     'zero live counters plus preserved snapshot space','Verify live GC separately from snapshot-retained space.','TEST_FAILURE')
        metrics=detail('foreground logical read and memory metrics')
        context=data.get('performance_context',{})
        self.require(context.get('cache_mode')=='remount_without_arc_flush' and
                     context.get('method')=='sha256_logical_read' and
                     context.get('run_id')==self.manifest['run_id'] and
                     context.get('ordinary_sha256')==context.get('optimized_sha256')==
                     detail('deterministic mixed fixtures').get('base_sha256'),
                     'performance_comparability',context,'same-run same-bytes remounted reads with hashing',
                     'Report benchmark/cache identity; historical or unequal workloads cannot form this gate.')
        values=[]
        for label in ('ordinary','optimized'):
            times=metrics.get(label,{}).get('cold_logical_read_seconds',[])
            self.require(len(times)==3 and all(positive(x) for x in times),'read_samples',times,
                         '3 finite positive read/hash times','Capture matched benchmark repetitions.')
            if len(times)==3 and all(positive(x) for x in times):
                values.append(sorted(times)[1])
        ratio=values[1]/values[0] if len(values)==2 else None
        self.require(ratio is not None and ratio<=10,'paired_read_slowdown',ratio,{'maximum':10},
                     'Investigate matched logical-read slowdown; do not weaken policy or compare different cache modes.','TEST_FAILURE')
        self.assessments['native']={'checks':len(checks),'fixture_savings':savings,'paired_remounted_read_slowdown':ratio,
                                   'write_memory':'context only; repeated writes hydrate only once and memory is sampled',
                                   'fresh_crash_legacy_coverage':False}

    def run(self):
        try:
            supplied=read_json(self.policy_path)
            if canonical(supplied)!=canonical(EXPECTED_POLICY):
                raise ValueError('policy content changed; acceptance policy cannot be weakened')
            self.manifest=read_json(self.root/'run.json')
            m=self.manifest
            if m.get('schema')!=1 or m.get('profile') not in EXPECTED_POLICY['profiles'] or not isinstance(m.get('run_id'),str):
                raise ValueError('invalid run manifest schema/profile/nonce')
            self.source()
            self.controls()
            for item in m['stages']:
                self.stage(item)
            self.current='stage_coverage'
            expected=EXPECTED_POLICY['local_stages']
            self.require(set(expected)<=set(self.stages),'local_stage_coverage',sorted(self.stages),expected,
                         'Execute every local prerequisite; missing stages are not passing evidence.')
            extras=set(self.stages)-set(expected)-set(EXPECTED_POLICY['native_stages'])
            self.require(not extras,'unknown_stages',sorted(extras),[],
                         'Resolve the unexpected driver failure; do not suppress it.')
            freeze=self.stages.get('source_freeze',{}).get('evidence',{})
            self.require(freeze.get('files_count')==len(m['source']['files']) and
                         freeze.get('tree_digest')==self.source_digest,'freeze_assertions',freeze,
                         'exact verified source count/digest','Regenerate the source-freeze receipt.')
            self.local()
            local_failed=any(self.stages.get(n,{}).get('status')!='PASS' for n in expected)
            if m['profile']!='local' and not local_failed:
                self.current='stage_coverage'
                self.require(set(EXPECTED_POLICY['native_stages'])<=set(self.stages),'native_stage_coverage',
                             sorted(self.stages),EXPECTED_POLICY['native_stages'],'Execute native stages for this exact run.')
                self.native()
            elif m['profile']!='local':
                self.assessments['native']='not executed: a local prerequisite failed; not current native proof'
            elif set(self.stages)&set(EXPECTED_POLICY['native_stages']):
                raise ValueError('local profile contains unexpected native execution')
            repairs=m.get('repair_history',[])
            self.require(isinstance(repairs,list) and len(repairs)<=3,'repair_bound',len(repairs),3,
                         'Stop after the bounded repair budget; retain unsuccessful evidence.')
        except (OSError,ValueError,KeyError,TypeError,IndexError,ZeroDivisionError) as error:
            self.fail('evidence_unusable',str(error),'complete current-run evidence',
                      'Inspect this artifact/schema/provenance error; do not claim success or repair source blindly.')
        categories={x['category'] for x in self.feedback}
        status='INFRA_FAILURE' if 'INFRA_FAILURE' in categories else 'TEST_FAILURE' if categories else 'PASS'
        result=dict(schema=1,run_id=self.manifest.get('run_id'),profile=self.manifest.get('profile'),status=status,
                    source_digest=getattr(self,'source_digest',None),policy_id=EXPECTED_POLICY['policy_id'],
                    evaluated_at=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    artifact_digests_checked=self.artifact_count,assessments=self.assessments,feedback=self.feedback,
                    claim='local models/conformance only' if self.manifest.get('profile')=='local' else
                          ('current guest 21-case regression; no fresh crash or old-software claim' if status=='PASS' else
                           'validation incomplete; no current native pass claim'))
        (self.root/'evaluation.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
        return {'PASS':0,'TEST_FAILURE':1,'INFRA_FAILURE':2}[status]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-dir',type=Path,required=True)
    parser.add_argument('--policy',type=Path,required=True)
    args=parser.parse_args()
    return Gate(args.run_dir,args.policy).run()


if __name__=='__main__':
    sys.exit(main())
