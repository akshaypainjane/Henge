import Foundation

/// Before iOS 27: which connected external screens show the running Mac. A screen not showing
/// it gets no window of ours, so iOS mirrors the phone there. Pure, so every order of session
/// and plug events can be tested; LegacyExternalDisplay carries out the changes it returns.
/// Screens are named by their surface id; a presentation is whatever the host shows.
struct ExternalScreenPlan<Presentation: Identifiable> {
    enum Change {
        case showMac(UUID, Presentation)
        case stopMac(UUID, Presentation)
    }

    private var presentation: Presentation?
    private var screens: [UUID] = []

    mutating func connect(_ surface: UUID) -> [Change] {
        screens.append(surface)
        guard let presentation else { return [] }
        return [.showMac(surface, presentation)]
    }

    mutating func disconnect(_ surface: UUID) -> [Change] {
        guard let index = screens.firstIndex(of: surface) else { return [] }
        screens.remove(at: index)
        guard let presentation else { return [] }
        return [.stopMac(surface, presentation)]
    }

    /// Every connected screen shows this presentation from now on, replacing any earlier one.
    mutating func begin(_ presentation: Presentation) -> [Change] {
        self.presentation = presentation
        return screens.map { .showMac($0, presentation) }
    }

    /// Only the presentation that began can end it: a late end from an older one (another id) changes nothing.
    mutating func end(_ presentation: Presentation) -> [Change] {
        guard let current = self.presentation, current.id == presentation.id else { return [] }
        self.presentation = nil
        return screens.map { .stopMac($0, current) }
    }
}
