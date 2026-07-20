// Action definitions. Every action maps to one wire command
// (docs/remote-control.md); input numbers are 1-based on the wire.

const TRANSITIONS = [
	{ id: 'mix', label: 'Mix' },
	{ id: 'wipelr', label: 'Wipe left-to-right' },
	{ id: 'wiperl', label: 'Wipe right-to-left' },
	{ id: 'wipetb', label: 'Wipe top-to-bottom' },
	{ id: 'wipebt', label: 'Wipe bottom-to-top' },
	{ id: 'wipebox', label: 'Box wipe' },
	{ id: 'wipecircle', label: 'Circle wipe' },
]

const DSK_CHOICES = [
	{ id: 1, label: 'DSK 1' },
	{ id: 2, label: 'DSK 2' },
]

const SET_CHOICES = [
	{ id: 'TOGGLE', label: 'Toggle' },
	{ id: 'ON', label: 'On' },
	{ id: 'OFF', label: 'Off' },
]

module.exports = function getActions(self) {
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
		choices: DSK_CHOICES,
	}

	return {
		cut: {
			name: 'Cut',
			options: [],
			callback: () => self.sendCmd('CUT'),
		},
		auto: {
			name: 'Auto transition',
			options: [],
			callback: () => self.sendCmd('AUTO'),
		},
		ftb: {
			name: 'Fade to black (toggle)',
			options: [],
			callback: () => self.sendCmd('FTB'),
		},
		program: {
			name: 'Set program input',
			options: [inputOption],
			callback: (a) => self.sendCmd(`PGM ${a.options.input}`),
		},
		preview: {
			name: 'Set preview input',
			options: [inputOption],
			callback: (a) => self.sendCmd(`PVW ${a.options.input}`),
		},
		transition: {
			name: 'Set transition type',
			options: [
				{
					type: 'dropdown',
					id: 'type',
					label: 'Type',
					default: 'mix',
					choices: TRANSITIONS,
				},
				{
					type: 'number',
					id: 'duration',
					label: 'Duration (frames, 0 = keep current)',
					default: 0,
					min: 0,
					max: 600,
				},
			],
			callback: (a) => {
				const dur = a.options.duration > 0 ? ` ${a.options.duration}` : ''
				self.sendCmd(`TRANSITION ${a.options.type}${dur}`)
			},
		},
		dsk: {
			name: 'DSK on air',
			options: [
				dskOption,
				{
					type: 'dropdown',
					id: 'mode',
					label: 'Action',
					default: 'TOGGLE',
					choices: SET_CHOICES,
				},
			],
			callback: (a) => self.sendCmd(`DSK ${a.options.dsk} ${a.options.mode}`),
		},
		dsk_source: {
			name: 'Set DSK fill source',
			options: [dskOption, inputOption],
			callback: (a) => self.sendCmd(`DSK ${a.options.dsk} SRC ${a.options.input}`),
		},
		dsk_fade: {
			name: 'Set DSK fade duration',
			options: [
				dskOption,
				{
					type: 'number',
					id: 'frames',
					label: 'Fade (frames, 0 = cut)',
					default: 30,
					min: 0,
					max: 600,
				},
			],
			callback: (a) => self.sendCmd(`DSK ${a.options.dsk} FADE ${a.options.frames}`),
		},
		media: {
			name: 'Media transport',
			options: [
				inputOption,
				{
					type: 'dropdown',
					id: 'op',
					label: 'Action',
					default: 'PLAY',
					choices: [
						{ id: 'PLAY', label: 'Play' },
						{ id: 'PAUSE', label: 'Pause' },
						{ id: 'RESTART', label: 'Restart' },
						{ id: 'NEXT', label: 'Next playlist item' },
						{ id: 'PREV', label: 'Previous playlist item' },
					],
				},
			],
			callback: (a) => self.sendCmd(`MEDIA ${a.options.input} ${a.options.op}`),
		},
		media_loop: {
			name: 'Media loop',
			options: [
				inputOption,
				{
					type: 'dropdown',
					id: 'mode',
					label: 'Loop',
					default: 'ON',
					choices: [
						{ id: 'ON', label: 'On' },
						{ id: 'OFF', label: 'Off' },
					],
				},
			],
			callback: (a) => self.sendCmd(`MEDIA ${a.options.input} LOOP ${a.options.mode}`),
		},
		record: {
			name: 'Record program',
			options: [
				{
					type: 'dropdown',
					id: 'mode',
					label: 'Action',
					default: 'TOGGLE',
					choices: [
						{ id: 'TOGGLE', label: 'Toggle' },
						{ id: 'START', label: 'Start' },
						{ id: 'STOP', label: 'Stop' },
					],
				},
				{
					type: 'textinput',
					id: 'path',
					label: 'Path (empty = ~/Videos, timestamped)',
					default: '',
				},
			],
			callback: (a) => {
				const path = a.options.mode !== 'STOP' && a.options.path ? ` ${a.options.path}` : ''
				self.sendCmd(`RECORD ${a.options.mode}${path}`)
			},
		},
		clean_record: {
			name: 'Record clean feed',
			options: [
				{
					type: 'dropdown',
					id: 'mode',
					label: 'Action',
					default: 'TOGGLE',
					choices: [
						{ id: 'TOGGLE', label: 'Toggle' },
						{ id: 'START', label: 'Start' },
						{ id: 'STOP', label: 'Stop' },
					],
				},
				{
					type: 'textinput',
					id: 'path',
					label: 'Path (empty = ~/Videos, timestamped)',
					default: '',
				},
			],
			callback: (a) => {
				const path = a.options.mode !== 'STOP' && a.options.path ? ` ${a.options.path}` : ''
				self.sendCmd(`CLEAN ${a.options.mode}${path}`)
			},
		},
		audio_mute: {
			name: 'Audio mute',
			options: [
				inputOption,
				{
					type: 'dropdown',
					id: 'mode',
					label: 'Action',
					default: 'TOGGLE',
					choices: SET_CHOICES,
				},
			],
			callback: (a) => self.sendCmd(`AUDIO ${a.options.input} MUTE ${a.options.mode}`),
		},
		audio_solo: {
			name: 'Audio solo',
			options: [
				inputOption,
				{
					type: 'dropdown',
					id: 'mode',
					label: 'Action',
					default: 'TOGGLE',
					choices: SET_CHOICES,
				},
			],
			callback: (a) => self.sendCmd(`AUDIO ${a.options.input} SOLO ${a.options.mode}`),
		},
		audio_gain: {
			name: 'Audio fader gain',
			options: [
				inputOption,
				{
					type: 'number',
					id: 'gain',
					label: 'Linear gain (1.0 = unity)',
					default: 1.0,
					min: 0,
					max: 4,
					step: 0.05,
				},
			],
			callback: (a) => self.sendCmd(`AUDIO ${a.options.input} GAIN ${a.options.gain}`),
		},
	}
}
