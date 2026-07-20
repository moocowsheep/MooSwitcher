// Drag-and-drop presets: program/preview rows with tally feedback, and a
// transport strip. Button text uses module variables so live input names
// appear once connected.
const { combineRgb } = require('@companion-module/base')

const WHITE = combineRgb(255, 255, 255)
const BLACK = combineRgb(0, 0, 0)

module.exports = function getPresets(self) {
	const presets = {}

	for (let n = 1; n <= self.inputCount(); ++n) {
		presets[`pgm_${n}`] = {
			type: 'button',
			category: 'Program',
			name: `Program ${n}`,
			style: { text: `$(mooswitcher:input_${n}_name)`, size: 'auto', color: WHITE, bgcolor: BLACK },
			steps: [{ down: [{ actionId: 'program', options: { input: n } }], up: [] }],
			feedbacks: [{ feedbackId: 'program', options: { input: n } }],
		}
		presets[`pvw_${n}`] = {
			type: 'button',
			category: 'Preview',
			name: `Preview ${n}`,
			style: { text: `$(mooswitcher:input_${n}_name)`, size: 'auto', color: WHITE, bgcolor: BLACK },
			steps: [{ down: [{ actionId: 'preview', options: { input: n } }], up: [] }],
			feedbacks: [{ feedbackId: 'preview', options: { input: n } }],
		}
	}

	presets.cut = {
		type: 'button',
		category: 'Transport',
		name: 'Cut',
		style: { text: 'CUT', size: '24', color: WHITE, bgcolor: BLACK },
		steps: [{ down: [{ actionId: 'cut', options: {} }], up: [] }],
		feedbacks: [],
	}
	presets.auto = {
		type: 'button',
		category: 'Transport',
		name: 'Auto',
		style: { text: 'AUTO', size: '24', color: WHITE, bgcolor: BLACK },
		steps: [{ down: [{ actionId: 'auto', options: {} }], up: [] }],
		feedbacks: [{ feedbackId: 'in_transition', options: {} }],
	}
	presets.ftb = {
		type: 'button',
		category: 'Transport',
		name: 'Fade to black',
		style: { text: 'FTB', size: '24', color: WHITE, bgcolor: BLACK },
		steps: [{ down: [{ actionId: 'ftb', options: {} }], up: [] }],
		feedbacks: [{ feedbackId: 'ftb', options: {} }],
	}
	for (const k of [1, 2]) {
		presets[`dsk_${k}`] = {
			type: 'button',
			category: 'Transport',
			name: `DSK ${k}`,
			style: { text: `DSK ${k}`, size: '18', color: WHITE, bgcolor: BLACK },
			steps: [{ down: [{ actionId: 'dsk', options: { dsk: k, mode: 'TOGGLE' } }], up: [] }],
			feedbacks: [{ feedbackId: 'dsk', options: { dsk: k } }],
		}
	}
	presets.record = {
		type: 'button',
		category: 'Transport',
		name: 'Record program',
		style: {
			text: 'REC\\n$(mooswitcher:record_time)',
			size: '14',
			color: WHITE,
			bgcolor: BLACK,
		},
		steps: [{ down: [{ actionId: 'record', options: { mode: 'TOGGLE', path: '' } }], up: [] }],
		feedbacks: [
			{ feedbackId: 'recording', options: {} },
			{ feedbackId: 'record_error', options: {} },
		],
	}
	presets.clean_record = {
		type: 'button',
		category: 'Transport',
		name: 'Record clean feed',
		style: {
			text: 'CLEAN\\n$(mooswitcher:clean_record_time)',
			size: '14',
			color: WHITE,
			bgcolor: BLACK,
		},
		steps: [{ down: [{ actionId: 'clean_record', options: { mode: 'TOGGLE', path: '' } }], up: [] }],
		feedbacks: [
			{ feedbackId: 'clean_recording', options: {} },
			{ feedbackId: 'record_error', options: {} },
		],
	}

	return presets
}
