import SwiftUI

extension RootView {
    var setsPane: some View {
        HStack(alignment: .top, spacing: 16) {
            List {
                ForEach(model.sets) { set in
                    Button { selected = set.id } label: {
                        HStack(spacing: 10) {
                            VStack(alignment: .leading, spacing: 4) {
                                Text(set.name).font(.system(size: 13, weight: .medium)).lineLimit(1)
                                Text("\(set.resources.count) resources")
                                    .font(.system(size: 11))
                                    .foregroundStyle(.secondary)
                            }
                            Spacer(minLength: 0)
                            if set
                                .activeEnd(at: now) !=
                                nil
                            {
                                Image(systemName: "lock.fill")
                                    .font(.system(size: 10))
                                    .foregroundStyle(.secondary)
                            }
                        }
                        .padding(12)
                        .background(
                            selected == set.id ? accent.opacity(0.1) : .clear,
                            in: RoundedRectangle(cornerRadius: 6)
                        )
                    }
                    .buttonStyle(HoverButtonStyle())
                    .listRowSeparator(.hidden)
                    .listRowBackground(Color.clear)
                    .contextMenu {
                        Button("Move up") { moveSet(set.id, by: -1) }
                            .disabled(model.sets.first?.id == set.id)
                        Button("Move down") { moveSet(set.id, by: 1) }
                            .disabled(model.sets.last?.id == set.id)
                    }
                }
                .onMove { model.sets.move(fromOffsets: $0, toOffset: $1) }
            }
            .listStyle(.plain)
            .scrollContentBackground(.hidden)
            .frame(width: 200)
            if let index {
                let locked = model.sets[index].activeEnd(at: now) != nil
                VStack(alignment: .leading, spacing: 22) {
                    HStack {
                        TextField("Set name", text: $model.sets[index].name)
                            .font(.system(size: 16, weight: .medium))
                            .textFieldStyle(.plain)
                            .disabled(locked)
                        if !locked {
                            Button(role: .destructive) { deleteSet(index) } label: {
                                Image(systemName: "trash")
                            }
                            .buttonStyle(HoverButtonStyle())
                            .foregroundStyle(.secondary)
                            .help("Delete set")
                        }
                    }
                    if let end = model.sets[index].activeEnd(at: now) {
                        Label(
                            "Until \(end.formatted(date: .omitted, time: .shortened))",
                            systemImage: "lock"
                        )
                        .font(.system(size: 12))
                        .foregroundStyle(accent)
                    }
                    ScrollView {
                        VStack(spacing: 0) {
                            ForEach(model.sets[index].resources) { resource in
                                HStack(spacing: 12) {
                                    Image(systemName: resource.symbol)
                                        .foregroundStyle(.secondary)
                                        .frame(width: 20)
                                    Text(resource.name).font(.system(size: 13))
                                    Spacer()
                                    if !locked {
                                        Button {
                                            model
                                                .edit(index) {
                                                    $0.resources.removeAll { $0.id == resource.id }
                                                }
                                        } label: { Image(systemName: "minus.circle") }
                                            .buttonStyle(HoverButtonStyle())
                                            .foregroundStyle(.tertiary)
                                            .help("Remove \(resource.name)")
                                    }
                                }.padding(16)
                                if resource.id != model.sets[index]
                                    .resources
                                    .last?
                                    .id
                                {
                                    Divider().padding(
                                        .leading,
                                        48
                                    )
                                }
                            }
                            if model.sets[index].resources.isEmpty {
                                emptyView(
                                    "Add a website or app",
                                    symbol: "plus.circle"
                                ).padding(24)
                            }
                        }.background(surface, in: RoundedRectangle(cornerRadius: 8))
                    }
                    HStack {
                        Button { resourceName = ""
                            resourceKind = .website
                        } label: {
                            Label("Website", systemImage: "plus")
                        }
                        Button { resourceKind = .app } label: {
                            Label("App", systemImage: "plus")
                        }
                    }.buttonStyle(ActionButtonStyle())
                }.frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            } else {
                emptyView("Create a set to get started", symbol: "square.stack").frame(
                    maxWidth: .infinity,
                    maxHeight: .infinity
                )
            }
        }
        .padding(.horizontal, 16)
        .padding(.bottom, 16)
    }

    func moveSet(_ id: String, by offset: Int) {
        guard let index = model.sets.firstIndex(where: { $0.id == id }),
              model.sets.indices.contains(index + offset)
        else {
            return
        }
        model.sets.swapAt(index, index + offset)
    }
}
