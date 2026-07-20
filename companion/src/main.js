// Bitfocus Companion module for MooSwitcher. Talks the line/JSON protocol on
// the switcher's TCP remote-control port (docs/remote-control.md; default
// 9923). State events drive feedbacks and variables; commands are one-line
// sends.
const { InstanceBase, InstanceStatus, runEntrypoint, TCPHelper } = require('@companion-module/base')
const UpgradeScripts = require('./upgrades')
const getActions = require('./actions')
const getFeedbacks = require('./feedbacks')
const { getVariableDefinitions, getVariableValues } = require('./variables')
const getPresets = require('./presets')

// Input count before the first state event arrives (the switcher frame is
// fixed at 21; dropdowns rebuild with live names once connected).
const DEFAULT_INPUTS = 21

class MooSwitcherInstance extends InstanceBase {
	constructor(internal) {
		super(internal)
		this.state = null
		this.socket = null
		this.recvBuffer = ''
	}

	async init(config) {
		this.config = config
		this.rebuildDefinitions()
		this.setPresetDefinitions(getPresets(this))
		this.initConnection()
	}

	async destroy() {
		this.dropConnection()
	}

	async configUpdated(config) {
		this.config = config
		this.dropConnection()
		this.initConnection()
	}

	getConfigFields() {
		return [
			{
				type: 'static-text',
				id: 'info',
				width: 12,
				label: 'Information',
				value:
					'Connects to the MooSwitcher remote-control port ' +
					'(GUI default 9923; moo-headless needs --control-port).',
			},
			{
				type: 'textinput',
				id: 'host',
				label: 'Switcher host',
				width: 8,
				default: '127.0.0.1',
			},
			{
				type: 'number',
				id: 'port',
				label: 'Port',
				width: 4,
				default: 9923,
				min: 1,
				max: 65535,
			},
		]
	}

	initConnection() {
		if (!this.config.host) {
			this.updateStatus(InstanceStatus.BadConfig, 'No host configured')
			return
		}
		this.updateStatus(InstanceStatus.Connecting)
		this.socket = new TCPHelper(this.config.host, this.config.port || 9923)
		this.socket.on('status_change', (status, message) => {
			this.updateStatus(status, message)
		})
		this.socket.on('connect', () => {
			this.recvBuffer = ''
			this.socket.send('SUBSCRIBE\n')
		})
		this.socket.on('data', (data) => {
			this.recvBuffer += data.toString('utf8')
			let nl
			while ((nl = this.recvBuffer.indexOf('\n')) >= 0) {
				const line = this.recvBuffer.slice(0, nl).trim()
				this.recvBuffer = this.recvBuffer.slice(nl + 1)
				if (line) this.handleEvent(line)
			}
		})
		this.socket.on('error', (err) => {
			this.log('error', `Connection error: ${err.message}`)
		})
	}

	dropConnection() {
		if (this.socket) {
			this.socket.destroy()
			this.socket = null
		}
		this.state = null
	}

	handleEvent(line) {
		let ev
		try {
			ev = JSON.parse(line)
		} catch (_e) {
			this.log('debug', `Unparseable event: ${line}`)
			return
		}
		switch (ev.event) {
			case 'hello':
				this.log('info', `Connected to ${ev.name} (protocol ${ev.protocol})`)
				this.updateStatus(InstanceStatus.Ok)
				break
			case 'state': {
				const prevNames = this.inputNames().join('\x00')
				const prevCount = this.inputCount()
				this.state = ev
				if (prevNames !== this.inputNames().join('\x00') || prevCount !== this.inputCount()) {
					this.rebuildDefinitions()
				}
				this.setVariableValues(getVariableValues(this))
				this.checkFeedbacks()
				break
			}
			case 'error':
				this.log('warn', `Switcher rejected a command: ${ev.message}`)
				break
			case 'pong':
				break
			default:
				this.log('debug', `Unknown event: ${ev.event}`)
		}
	}

	// Rebuilds everything whose option dropdowns embed input names.
	rebuildDefinitions() {
		this.setActionDefinitions(getActions(this))
		this.setFeedbackDefinitions(getFeedbacks(this))
		this.setVariableDefinitions(getVariableDefinitions(this))
		this.setVariableValues(getVariableValues(this))
	}

	sendCmd(cmd) {
		if (!this.socket || !this.socket.isConnected) {
			this.log('warn', `Not connected; dropped: ${cmd}`)
			return
		}
		this.socket.send(cmd + '\n')
	}

	inputCount() {
		return this.state ? this.state.inputs.length : DEFAULT_INPUTS
	}

	inputNames() {
		if (!this.state) return []
		return this.state.inputs.map((i) => i.ref)
	}

	// Dropdown choices, 1-based to match the wire protocol and the GUI.
	inputChoices() {
		const choices = []
		for (let n = 1; n <= this.inputCount(); ++n) {
			const ref = this.state ? this.state.inputs[n - 1].ref : ''
			choices.push({ id: n, label: ref ? `${n}: ${ref}` : `${n}` })
		}
		return choices
	}
}

runEntrypoint(MooSwitcherInstance, UpgradeScripts)
