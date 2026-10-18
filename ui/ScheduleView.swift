import SwiftUI

extension RootView {
    @ViewBuilder var schedulePane: some View {
        if model.sets.isEmpty {
            emptyView("Create a set to schedule", symbol: "calendar")
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            ScrollView {
                LazyVStack(spacing: 14) {
                    ForEach(model.sets.indices, id: \.self) { i in
                        scheduleCard(i)
                    }
                }.padding(.horizontal, 16).padding(.bottom, 24)
            }
        }
    }

    func scheduleCard(_ i: Int) -> some View {
        let set = model.sets[i]
        let locked = set.activeEnd(at: now) != nil
        return VStack(alignment: .leading, spacing: 18) {
            HStack {
                Text(set.name).font(.system(size: 13, weight: .medium))
                Spacer()
                if let end = set.activeEnd(at: now) {
                    Label("Until \(end.formatted(date: .omitted, time: .shortened))", systemImage: "lock.fill")
                        .font(.system(size: 11)).foregroundStyle(accent)
                }
                if !set.windows.isEmpty {
                    Button { scheduleToClear = set.id } label: {
                        Image(systemName: "trash").font(.system(size: 12)).frame(width: 24, height: 24)
                    }.buttonStyle(HoverButtonStyle()).foregroundStyle(.secondary)
                        .disabled(locked).help("Clear weekly schedule")
                        .accessibilityLabel("Clear schedule for \(set.name)")
                }
            }
            HStack(spacing: 9) {
                ForEach(0..<7) { day in
                    let configured = set.windows.contains { $0.day == day }
                    let focused = expanded == set.id && selectedDay == day
                    Button { expanded = set.id; selectedDay = day } label: {
                        VStack(spacing: 6) {
                            Text(days[day]).font(.system(size: 12, weight: .medium))
                            Circle().fill(configured ? accent : .clear).frame(width: 4, height: 4)
                        }.frame(maxWidth: .infinity).padding(.top, 11).padding(.bottom, 7)
                            .background(configured ? accent.opacity(0.09) : background, in: RoundedRectangle(cornerRadius: 8))
                            .overlay(RoundedRectangle(cornerRadius: 8).stroke(focused ? accent : .clear, lineWidth: 1.5))
                    }.buttonStyle(HoverButtonStyle()).accessibilityLabel("\(days[day]), \(set.name)")
                }
            }
            if expanded == set.id {
                Divider()
                HStack {
                    Text(Calendar.current.weekdaySymbols[selectedDay]).font(.system(size: 12, weight: .semibold))
                    Spacer()
                    if !locked {
                        Button {
                            guard model.sets[i].activeEnd(at: Date()) == nil else { return }
                            model.edit(i) { $0.windows.append(WeeklyWindow(day: selectedDay, start: 13 * 60, end: 15 * 60)) }
                        } label: { Label("Time", systemImage: "plus") }.buttonStyle(HoverButtonStyle()).foregroundStyle(accent).font(.system(size: 12))
                    }
                }
                ForEach(set.windows.filter { $0.day == selectedDay }) { window in
                    HStack(spacing: 8) {
                        TimeInput(value: timeBinding(i, window.id, false), label: "Start").frame(width: 88, height: 30)
                        Text("—").foregroundStyle(.tertiary)
                        TimeInput(value: timeBinding(i, window.id, true), label: "End").frame(width: 88, height: 30)
                        if window.end <= window.start { Text("Next day").font(.system(size: 10)).foregroundStyle(.secondary) }
                        Spacer()
                        if !locked {
                            Button { model.edit(i) { $0.windows.removeAll { $0.id == window.id } } } label: { Image(systemName: "minus.circle") }.buttonStyle(HoverButtonStyle()).foregroundStyle(.secondary).help("Remove time range")
                        }
                    }.disabled(locked)
                }
                if !set.windows.contains(where: { $0.day == selectedDay }) {
                    Text("No blocks").font(.system(size: 12)).foregroundStyle(.tertiary).padding(.vertical, 3)
                }
            }
        }.padding(22).background(surface, in: RoundedRectangle(cornerRadius: 14))
    }

}
