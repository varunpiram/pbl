import SwiftUI

extension RootView {
    var quickPane: some View {
        VStack(spacing: 24) {
            if let index {
                let set = model.sets[index]
                let active = set.activeInterval(at: now)
                let end = active?.end
                Picker("Set", selection: $selected) {
                    ForEach(model.sets) { Text($0.name).tag($0.id) }
                }
                .pickerStyle(.menu)
                .labelsHidden()
                .buttonStyle(.borderless)
                .font(.system(size: 13))
                .frame(width: 230)
                Text(resourceSummary(set))
                    .font(.system(size: 12))
                    .foregroundStyle(.secondary)
                    .padding(.top, -14)
                Group {
                    if let active {
                        TimelineView(.periodic(from: .now, by: 1)) { context in
                            ZStack {
                                DurationDial(
                                    minutes: $minutes,
                                    remainingFraction: min(1, max(
                                        0,
                                        active.end.timeIntervalSince(context.date) / (12 * 60 * 60)
                                    )),
                                    accent: accent
                                )
                                VStack(spacing: 8) {
                                    Image(systemName: "lock.fill")
                                        .font(.system(size: 15))
                                        .foregroundStyle(accent)
                                    Text(countdown(active.end, at: context.date))
                                        .font(.system(size: 34))
                                        .monospacedDigit()
                                }.allowsHitTesting(false)
                            }
                        }
                    } else {
                        ZStack {
                            DurationDial(minutes: $minutes, remainingFraction: nil, accent: accent)
                            Text(durationLabel(minutes))
                                .font(.system(size: 36))
                                .monospacedDigit()
                                .allowsHitTesting(false)
                        }
                    }
                }
                .frame(width: 280, height: 280)
                .padding(.vertical, 4)
                if let end {
                    Label(
                        "Until \(end.formatted(date: .omitted, time: .shortened))",
                        systemImage: "lock"
                    )
                    .font(.system(size: 13, weight: .medium))
                    .foregroundStyle(accent)
                } else {
                    HStack(spacing: 10) {
                        ForEach([30, 60, 120, 360], id: \.self) { value in
                            Button { minutes = Double(value) } label: {
                                Text(value < 60 ? "\(value) min" : "\(value / 60)h")
                                    .font(.system(size: 12, weight: .medium))
                                    .padding(.horizontal, 14)
                                    .padding(.vertical, 8)
                                    .background(
                                        minutes == Double(value) ? accent.opacity(0.13) : surface,
                                        in: RoundedRectangle(cornerRadius: 5)
                                    )
                                    .foregroundStyle(minutes == Double(value) ? accent : .secondary)
                            }.buttonStyle(HoverButtonStyle())
                        }
                    }
                    Button {
                        guard model.sets[index].activeEnd(at: Date()) == nil else {
                            return
                        }
                        let duration = min(720, max(0, (minutes / 15).rounded() * 15))
                        guard duration > 0 else {
                            return
                        }
                        model.send([
                            "command": "submit",
                            "id": model.sets[index].id,
                            "minutes": Int(duration)
                        ])
                    } label: {
                        HStack(spacing: 8) {
                            if model.busy {
                                ProgressView().controlSize(.small)
                            }
                            Text("Start block")
                        }
                        .font(.system(size: 13, weight: .medium))
                        .frame(width: 236)
                        .padding(
                            .vertical,
                            12
                        )
                    }
                    .buttonStyle(ActionButtonStyle())
                    .disabled(set.resources.isEmpty || minutes <= 0)
                }
            } else {
                emptyView("Your first block starts with a set", symbol: "square.stack")
                Button("Create set") { addSet()
                    pane = .sets
                }.buttonStyle(ActionButtonStyle())
            }
        }.frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .center)
    }
}
