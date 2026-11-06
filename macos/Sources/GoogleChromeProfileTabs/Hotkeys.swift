import Carbon.HIToolbox

/// Ctrl+1...9 switch to the tab with that number. The keys are taken only while the browser is in front,
/// so other apps keep them.
@MainActor final class Hotkeys {
    var onPress: ((Int) -> Void)?

    private var registered: [EventHotKeyRef] = []
    private var handler: EventHandlerRef?
    private static let signature: OSType = 0x5054_6162 // 'PTab'
    private static let keys = [
        kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4, kVK_ANSI_5,
        kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9,
    ]

    init() {
        var spec = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))
        InstallEventHandler(GetApplicationEventTarget(), { _, event, refcon in
            guard let event, let refcon else { return OSStatus(eventNotHandledErr) }
            var id = EventHotKeyID()
            let status = GetEventParameter(
                event,
                EventParamName(kEventParamDirectObject),
                EventParamType(typeEventHotKeyID),
                nil,
                MemoryLayout<EventHotKeyID>.size,
                nil,
                &id
            )
            guard status == noErr, id.signature == Hotkeys.signature else { return OSStatus(eventNotHandledErr) }
            let hotkeys = Unmanaged<Hotkeys>.fromOpaque(refcon).takeUnretainedValue()
            let number = Int(id.id)
            MainActor.assumeIsolated { hotkeys.onPress?(number) }
            return noErr
        }, 1, &spec, Unmanaged.passUnretained(self).toOpaque(), &handler)
    }

    func setEnabled(_ enabled: Bool) {
        if enabled == !registered.isEmpty { return }
        if enabled {
            for (index, key) in Self.keys.enumerated() {
                var ref: EventHotKeyRef?
                let id = EventHotKeyID(signature: Self.signature, id: UInt32(index + 1))
                if RegisterEventHotKey(UInt32(key), UInt32(controlKey), id, GetApplicationEventTarget(), 0, &ref) == noErr,
                   let ref {
                    registered.append(ref)
                }
            }
        } else {
            registered.forEach { UnregisterEventHotKey($0) }
            registered.removeAll()
        }
    }
}
