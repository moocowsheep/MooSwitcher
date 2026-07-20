// Variables exposed to button text and triggers.

function recTime(rec, fps) {
	if (!rec || !rec.active || !fps) return '--:--:--'
	const total = Math.floor(rec.frames / fps)
	const h = String(Math.floor(total / 3600)).padStart(2, '0')
	const m = String(Math.floor((total % 3600) / 60)).padStart(2, '0')
	const s = String(total % 60).padStart(2, '0')
	return `${h}:${m}:${s}`
}

function inputName(state, n) {
	const i = state && state.inputs[n - 1]
	return i && i.ref ? i.ref : `Input ${n}`
}

function getVariableDefinitions(self) {
	const defs = [
		{ variableId: 'program', name: 'Program input number' },
		{ variableId: 'program_name', name: 'Program input name' },
		{ variableId: 'preview', name: 'Preview input number' },
		{ variableId: 'preview_name', name: 'Preview input name' },
		{ variableId: 'transition', name: 'Transition type' },
		{ variableId: 'record_state', name: 'Program record state' },
		{ variableId: 'record_time', name: 'Program record time (hh:mm:ss)' },
		{ variableId: 'clean_record_state', name: 'Clean record state' },
		{ variableId: 'clean_record_time', name: 'Clean record time (hh:mm:ss)' },
		{ variableId: 'srt_state', name: 'SRT output state' },
	]
	for (let n = 1; n <= self.inputCount(); ++n) {
		defs.push({ variableId: `input_${n}_name`, name: `Input ${n} name` })
	}
	return defs
}

function getVariableValues(self) {
	const s = self.state
	const recState = (r) => (!r ? 'unknown' : r.error ? 'error' : r.active ? 'recording' : r.pending ? 'starting' : 'idle')
	const values = {
		program: s ? s.program : 0,
		program_name: s ? inputName(s, s.program) : '',
		preview: s ? s.preview : 0,
		preview_name: s ? inputName(s, s.preview) : '',
		transition: s ? s.transition : '',
		record_state: recState(s && s.record),
		record_time: s ? recTime(s.record, s.fps) : '--:--:--',
		clean_record_state: recState(s && s.cleanRecord),
		clean_record_time: s ? recTime(s.cleanRecord, s.fps) : '--:--:--',
		srt_state: !s || !s.srt.configured ? 'off' : s.srt.connected ? 'connected' : 'listening',
	}
	for (let n = 1; n <= self.inputCount(); ++n) {
		values[`input_${n}_name`] = inputName(s, n)
	}
	return values
}

module.exports = { getVariableDefinitions, getVariableValues }
