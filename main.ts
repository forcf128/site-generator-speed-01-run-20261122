// Zen driver: the harness runs `node /bot/main.ts` (node22). It reads every
// case from /input/cases.json, answers each one, and writes
// /output/answers.json: a JSON array of the answers, one per case and in the
// cases' order. Outside a container MAKEFUN_INPUT_DIR and MAKEFUN_OUTPUT_DIR
// stand in for /input and /output. Written from scratch.
import { join } from 'node:path';
import { runBatch } from './solve.js';

// Note: no setFlagsFromString('--no-incremental-marking') here. The house
// baseline sets it, but this entry does almost no allocation in JS (the
// work is native), and locally the flag costs ~5 ms of cold-process CPU.
const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

runBatch(join(INPUT_DIR, 'cases.json'), join(OUTPUT_DIR, 'answers.json'));
