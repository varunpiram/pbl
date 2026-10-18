import SwiftUI
import AppKit

enum AppPalette {
    static func background(_ scheme: ColorScheme) -> Color {
        scheme == .dark ? Color(red: 0.12, green: 0.115, blue: 0.105) : Color(red: 0.96, green: 0.94, blue: 0.90)
    }
    static func surface(_ scheme: ColorScheme) -> Color {
        scheme == .dark ? Color(red: 0.16, green: 0.155, blue: 0.145) : Color(red: 0.94, green: 0.915, blue: 0.87)
    }
}

struct DurationDial: View {
    @Binding var minutes: Double
    var remainingFraction: Double?
    @State private var previousAngle: Double?
    @State private var dragMinutes = 0.0
    private var disabled: Bool { remainingFraction != nil }
    var accent: Color
    private var fraction: Double { min(1, max(0, minutes / 720)) }
    var body: some View {
        GeometryReader { geometry in
            let diameter = geometry.size.width
            let radius = diameter / 2 - 14
            ZStack {
                Circle().stroke(accent.opacity(0.1), lineWidth: 4)
                Circle().trim(from: 0, to: remainingFraction ?? fraction)
                    .stroke(accent, style: StrokeStyle(lineWidth: 4, lineCap: .round)).rotationEffect(.degrees(-90))
                if !disabled {
                    Circle().fill(accent).frame(width: 14, height: 14)
                        .overlay(Circle().stroke(accent.opacity(0.15), lineWidth: 6))
                        .offset(x: radius * sin(fraction * 2 * .pi), y: -radius * cos(fraction * 2 * .pi))
                }
            }.padding(14)
                .contentShape(Circle().stroke(lineWidth: 40))
                .gesture(DragGesture(minimumDistance: 0).onChanged { value in
                    guard !disabled else { return }
                    let dx = value.location.x - diameter / 2
                    let dy = value.location.y - diameter / 2
                    var angle = Double(atan2(dx, -dy))
                    if angle < 0 { angle += 2 * .pi }
                    if let previousAngle {
                        var delta = angle - previousAngle
                        if delta > .pi { delta -= 2 * .pi }
                        if delta < -.pi { delta += 2 * .pi }
                        dragMinutes = min(720, max(0, dragMinutes + delta / (2 * .pi) * 720))
                    } else {
                        let position = angle / (2 * .pi) * 720
                        dragMinutes = abs(position - minutes) > 360 ? minutes : position
                    }
                    previousAngle = angle
                    minutes = min(720, max(0, (dragMinutes / 15).rounded() * 15))
                }.onEnded { _ in previousAngle = nil })
                .accessibilityElement().accessibilityLabel("Block duration")
                .accessibilityValue(disabled ? "Countdown" : "\(Int(minutes)) minutes")
                .accessibilityAdjustableAction { direction in
                    guard !disabled else { return }
                    minutes = min(720, max(0, minutes + (direction == .increment ? 15 : -15)))
                }
        }
    }
}

struct WindowTheme: NSViewRepresentable {
    var dark: Bool
    func makeNSView(context: Context) -> WindowThemeView { WindowThemeView() }
    func updateNSView(_ view: WindowThemeView, context: Context) { view.dark = dark }
}

final class WindowThemeView: NSView {
    var dark = false { didSet { if dark != oldValue { updateTitlebar() } } }
    override func viewDidMoveToWindow() { updateTitlebar() }
    private func updateTitlebar() {
        guard let window else { return }
        window.titlebarAppearsTransparent = false
        window.backgroundColor = dark ? NSColor(calibratedRed: 0.16, green: 0.155, blue: 0.145, alpha: 1) : NSColor(calibratedRed: 0.91, green: 0.90, blue: 0.875, alpha: 1)
    }
}

enum ResourceKind: String, Identifiable {
    case website, app
    var id: Self { self }
}

struct ActionButtonStyle: ButtonStyle {
    @Environment(\.colorScheme) private var scheme
    @Environment(\.isEnabled) private var enabled
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 13, weight: .medium))
            .padding(.horizontal, 14).padding(.vertical, 8)
            .foregroundStyle(Color.primary.opacity(0.85))
            .background(Color.primary.opacity(scheme == .dark ? 0.09 : 0.06), in: RoundedRectangle(cornerRadius: 6))
            .overlay {
                RoundedRectangle(cornerRadius: 6)
                    .fill(Color.primary.opacity(configuration.isPressed ? 0.08 : 0))
            }
            .opacity(enabled ? 1 : 0.4)
            .modifier(ActionHover())
    }
}

private struct ActionHover: ViewModifier {
    @State private var hovering = false
    func body(content: Content) -> some View {
        content.overlay {
            RoundedRectangle(cornerRadius: 6).fill(Color.primary.opacity(hovering ? 0.04 : 0)).allowsHitTesting(false)
        }.onHover { hovering = $0 }
    }
}

struct HoverButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        HoverButtonBody(configuration: configuration)
    }
}

private struct HoverButtonBody: View {
    let configuration: ButtonStyle.Configuration
    @State private var hovering = false
    @Environment(\.isEnabled) private var enabled
    var body: some View {
        configuration.label
            .contentShape(Rectangle())
            .overlay {
                RoundedRectangle(cornerRadius: 5)
                    .fill(Color.primary.opacity(enabled ? (configuration.isPressed ? 0.14 : hovering ? 0.07 : 0) : 0))
                    .allowsHitTesting(false)
            }
            .onHover { hovering = $0 }
    }
}


struct TimeInput: View {
    @Environment(\.colorScheme) private var scheme
    @Binding var value: Int
    var label: String
    @State private var draft = ""
    @State private var showingChoices = false
    @State private var hovering = false
    @State private var invalid = false
    @FocusState private var focused: Bool
    @Environment(\.isEnabled) private var enabled
    private static var formatter: DateFormatter {
        let formatter = DateFormatter()
        formatter.locale = .autoupdatingCurrent
        formatter.timeZone = TimeZone(secondsFromGMT: 0)
        formatter.timeStyle = .short
        return formatter
    }
    private func formatted(_ minutes: Int) -> String {
        Self.formatter.string(from: Date(timeIntervalSinceReferenceDate: Double(minutes * 60)))
    }
    private func parsed(_ text: String) -> Int? {
        let text = text.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "\u{202f}", with: " ")
        let formatter = Self.formatter
        var date = formatter.date(from: text)
        if date == nil {
            formatter.locale = Locale(identifier: "en_US_POSIX")
            for format in ["H:mm", "h:mm a", "h:mma", "h a", "ha"] {
                formatter.dateFormat = format
                if let result = formatter.date(from: text) { date = result; break }
            }
        }
        guard let date else { return nil }
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(secondsFromGMT: 0)!
        return calendar.component(.hour, from: date) * 60 + calendar.component(.minute, from: date)
    }
    private func commit() {
        if let minutes = parsed(draft) {
            value = minutes
            draft = formatted(minutes)
            invalid = false
        } else { invalid = true }
    }
    var body: some View {
        HStack(spacing: 2) {
            TextField(label, text: $draft)
                .textFieldStyle(.plain).focused($focused)
                .font(.system(size: 13)).monospacedDigit()
                .foregroundStyle(invalid ? Color.red : Color.primary)
                .accessibilityLabel(label)
                .onSubmit { commit(); if !invalid { focused = false } }
            Button { focused = false; showingChoices.toggle() } label: {
                Image(systemName: "chevron.down").font(.system(size: 9, weight: .medium))
                    .foregroundStyle(.secondary).frame(width: 14, height: 26)
            }.buttonStyle(HoverButtonStyle()).accessibilityLabel("Choose \(label.lowercased()) time")
                .popover(isPresented: $showingChoices, arrowEdge: .bottom) {
                    ScrollViewReader { proxy in
                        ScrollView {
                            LazyVStack(spacing: 2) {
                                ForEach(Array(stride(from: 0, to: 1440, by: 15)), id: \.self) { minute in
                                    Button {
                                        value = minute; draft = formatted(minute); invalid = false; showingChoices = false
                                    } label: {
                                        HStack {
                                            Text(formatted(minute)).font(.system(size: 13)).monospacedDigit()
                                            Spacer()
                                            if minute == value { Image(systemName: "checkmark").font(.system(size: 10)) }
                                        }.padding(.horizontal, 10).padding(.vertical, 7)
                                    }.buttonStyle(HoverButtonStyle()).id(minute)
                                }
                            }.padding(6)
                        }.frame(width: 150, height: 230).background(AppPalette.background(scheme))
                            .onAppear { proxy.scrollTo(min(1425, value / 15 * 15), anchor: .center) }
                    }
                }
        }
        .padding(.leading, 4).padding(.trailing, 2)
        .background(Color.primary.opacity(enabled && (hovering || focused || showingChoices) ? 0.055 : 0), in: RoundedRectangle(cornerRadius: 4))
        .onHover { hovering = $0 }
        .onAppear { draft = formatted(value) }
        .onChange(of: value) { _, value in if !focused { draft = formatted(value) } }
        .onChange(of: focused) { _, focused in
            if !focused { commit(); if invalid { draft = formatted(value); invalid = false } }
        }
        .help(invalid ? "Enter a time such as 9:10 AM or 14:30" : label)
    }
}
