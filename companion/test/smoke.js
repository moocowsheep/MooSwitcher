// Integration smoke test: loads the real module sources with a stubbed
// Companion host (InstanceBase/runEntrypoint replaced; TCPHelper is the real
// one) and drives a live switcher. Run a switcher first, then:
//   node test/smoke.js [host] [port]
// Exits 0 when every check passes.
const assert = require('assert')
const Module = require('module')

const real = require('@companion-module/base')

let EntryClass = null
class StubInstanceBase {
	constructor() {
		this._status = null
		this._log = []
		this._actions = {}
		this._feedbacks = {}
		this._variableDefs = []
		this._variableValues = {}
		this._presets = {}
	}
	updateStatus(status, message) {
		this._status = status
		if (message) this._log.push(['status', message])
	}
	log(level, message) {
		this._log.push([level, message])
	}
	setActionDefinitions(d) {
		this._actions = d
	}
	setFeedbackDefinitions(d) {
		this._feedbacks = d
	}
	setVariableDefinitions(d) {
		this._variableDefs = d
	}
	setVariableValues(v) {
		Object.assign(this._variableValues, v)
	}
	setPresetDefinitions(d) {
		this._presets = d
	}
	checkFeedbacks() {}
}

const stub = {
	...real,
	InstanceBase: StubInstanceBase,
	runEntrypoint: (cls) => {
		EntryClass = cls
	},
}
const origLoad = Module._load
Module._load = function (request, ...rest) {
	if (request === '@companion-module/base') return stub
	return origLoad.call(this, request, ...rest)
}
require('../src/main.js')
assert(EntryClass, 'runEntrypoint was not called')

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
const host = process.argv[2] || '127.0.0.1'
const port = Number(process.argv[3] || 9923)

async function main() {
	const inst = new EntryClass()
	await inst.init({ host, port })

	await sleep(1500)
	assert.strictEqual(inst._status, real.InstanceStatus.Ok, `status ${inst._status}`)
	assert(inst.state, 'no state event received')
	assert(inst.state.inputs.length >= 2, 'expected at least 2 inputs')
	assert(inst._actions.cut && inst._actions.program, 'actions missing')
	assert(inst._feedbacks.program && inst._feedbacks.preview, 'feedbacks missing')
	assert(inst._presets.cut, 'presets missing')
	assert.strictEqual(inst._variableValues.input_1_name, inst.state.inputs[0].ref)
	console.log('connected; program =', inst.state.program, 'preview =', inst.state.preview)

	// Actions must reach the engine and come back as state + feedback flips.
	inst._actions.preview.callback({ options: { input: 1 } })
	inst._actions.program.callback({ options: { input: 2 } })
	await sleep(500)
	assert.strictEqual(inst.state.program, 2, 'PGM 2 did not land')
	assert.strictEqual(inst.state.preview, 1, 'PVW 1 did not land')
	assert(inst._feedbacks.program.callback({ options: { input: 2 } }), 'pgm tally')
	assert(!inst._feedbacks.program.callback({ options: { input: 1 } }), 'pgm anti-tally')
	assert(inst._feedbacks.preview.callback({ options: { input: 1 } }), 'pvw tally')
	assert.strictEqual(inst._variableValues.program, 2)

	inst._actions.cut.callback({})
	await sleep(500)
	assert.strictEqual(inst.state.program, 1, 'cut did not swap')
	assert.strictEqual(inst.state.preview, 2, 'cut did not swap preview')

	inst._actions.dsk.callback({ options: { dsk: 1, mode: 'ON' } })
	await sleep(800)
	assert(inst._feedbacks.dsk.callback({ options: { dsk: 1 } }), 'dsk1 feedback')
	inst._actions.dsk.callback({ options: { dsk: 1, mode: 'OFF' } })

	// A rejected command must surface as a warn log, not kill anything.
	inst.sendCmd('PGM 99')
	await sleep(300)
	assert(
		inst._log.some(([lvl, msg]) => lvl === 'warn' && msg.includes('out of range')),
		'engine error not logged'
	)

	await inst.destroy()
	console.log('smoke: all checks passed')
	process.exit(0)
}

main().catch((e) => {
	console.error('smoke FAILED:', e.message)
	process.exit(1)
})
