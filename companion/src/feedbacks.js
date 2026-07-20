// Boolean feedbacks driven by the subscribed state events. Colors follow
// broadcast tally conventions: red program, green preview, amber alerts.
const { combineRgb } = require('@companion-module/base')

const RED = combineRgb(200, 0, 0)
const GREEN = combineRgb(0, 160, 0)
const AMBER = combineRgb(255, 140, 0)
const WHITE = combineRgb(255, 255, 255)
const BLACK = combineRgb(0, 0, 0)

module.exports = function getFeedbacks(self) {
	const inputOption = {
		type: 'dropdown',
		id: 'input',
		label: 'Input',
		default: 1,
		choices: self.inputChoices(),
	}
	const dskOption = {
		type: 'dropdown',
		id: 'dsk',
		label: 'Keyer',
		default: 1,
		choices: [
			{ id: 1, label: 'DSK 1' },
			{ id: 2, label: 'DSK 2' },
		],
	}

	return {
		program: {
			type: 'boolean',
			name: 'Input is on program',
			defaultStyle: { bgcolor: RED, color: WHITE },
			options: [inputOption],
			callback: (fb) => self.state !== null && self.state.program === Number(fb.options.input),
		},
		preview: {
			type: 'boolean',
			name: 'Input is on preview',
			defaultStyle: { bgcolor: GREEN, color: WHITE },
			options: [inputOption],
			callback: (fb) => self.state !== null && self.state.preview === Number(fb.options.input),
		},
		in_transition: {
			type: 'boolean',
			name: 'Transition in progress',
			defaultStyle: { bgcolor: AMBER, color: BLACK },
			options: [],
			callback: () => self.state !== null && self.state.inTransition,
		},
		ftb: {
			type: 'boolean',
			name: 'Fade to black engaged',
			defaultStyle: { bgcolor: AMBER, color: BLACK },
			options: [],
			callback: () => self.state !== null && self.state.ftb,
		},
		dsk: {
			type: 'boolean',
			name: 'DSK on air',
			defaultStyle: { bgcolor: RED, color: WHITE },
			options: [dskOption],
			callback: (fb) => {
				const k = self.state && self.state.dsk[Number(fb.options.dsk) - 1]
				return k ? k.on : false
			},
		},
		dsk_tie: {
			type: 'boolean',
			name: 'DSK tied to transition',
			defaultStyle: { bgcolor: AMBER, color: BLACK },
			options: [dskOption],
			callback: (fb) => {
				const k = self.state && self.state.dsk[Number(fb.options.dsk) - 1]
				return k ? !!k.tie : false
			},
		},
		dsk_afv: {
			type: 'boolean',
			name: 'DSK audio follow enabled',
			defaultStyle: { bgcolor: AMBER, color: BLACK },
			options: [dskOption],
			callback: (fb) => {
				const k = self.state && self.state.dsk[Number(fb.options.dsk) - 1]
				return k ? !!k.afv : false
			},
		},
		recording: {
			type: 'boolean',
			name: 'Program recording active',
			defaultStyle: { bgcolor: RED, color: WHITE },
			options: [],
			callback: () => self.state !== null && (self.state.record.active || self.state.record.pending),
		},
		clean_recording: {
			type: 'boolean',
			name: 'Clean-feed recording active',
			defaultStyle: { bgcolor: RED, color: WHITE },
			options: [],
			callback: () =>
				self.state !== null && (self.state.cleanRecord.active || self.state.cleanRecord.pending),
		},
		record_error: {
			type: 'boolean',
			name: 'A recording is in error',
			defaultStyle: { bgcolor: AMBER, color: BLACK },
			options: [],
			callback: () => self.state !== null && (self.state.record.error || self.state.cleanRecord.error),
		},
		srt_connected: {
			type: 'boolean',
			name: 'SRT output connected',
			defaultStyle: { bgcolor: GREEN, color: WHITE },
			options: [],
			callback: () => self.state !== null && self.state.srt.connected,
		},
		input_down: {
			type: 'boolean',
			name: 'Assigned input has no signal',
			defaultStyle: { bgcolor: AMBER, color: BLACK },
			options: [inputOption],
			callback: (fb) => {
				const i = self.state && self.state.inputs[Number(fb.options.input) - 1]
				return i ? i.ref !== '' && !i.connected : false
			},
		},
		media_playing: {
			type: 'boolean',
			name: 'Media input is playing',
			defaultStyle: { bgcolor: GREEN, color: WHITE },
			options: [inputOption],
			callback: (fb) => {
				const i = self.state && self.state.inputs[Number(fb.options.input) - 1]
				return i && i.media ? i.media.playing : false
			},
		},
		audio_muted: {
			type: 'boolean',
			name: 'Audio input is muted',
			defaultStyle: { bgcolor: AMBER, color: BLACK },
			options: [inputOption],
			callback: (fb) => {
				const i = self.state && self.state.inputs[Number(fb.options.input) - 1]
				return i ? !!i.mute : false
			},
		},
	}
}
