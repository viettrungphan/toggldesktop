//
//  TimelineTimeEntry.swift
//  TogglDesktop
//
//  Created by Nghia Tran on 6/21/19.
//  Copyright © 2019 Alari. All rights reserved.
//

import Foundation

class TimelineBaseTimeEntry {

    struct Constant {
        static let MinimumSecondOverlap: Double = 60 // seconds
    }

    var start: TimeInterval
    var end: TimeInterval

    // Group of overlap entries -> Help to resolve the overlap later
    private(set) var group: Int = -1

    // The number column of the Timeline Entry. The first col is 0
    private(set) var col: Int = 0

    var isOverlap: Bool {
        return col > 0
    }

    private(set) var hasDetailInfo = false

    init(start: TimeInterval, end: TimeInterval, offset: TimeInterval = 0) {
        self.start = start + offset
        self.end = end - offset
    }

    func timechunk() -> TimeChunk {
        return TimeChunk(start: start, end: end)
    }

    func isIntersected(with entry: TimelineBaseTimeEntry) -> Bool {

        // Skip overlap if the diff is less than 60s
        if abs(entry.start - end) <= Constant.MinimumSecondOverlap ||
            abs(entry.end - start) <= Constant.MinimumSecondOverlap {
            return false
        }

        // Convert to rect to easier check intersects
        let currentRect = CGRect(x: 1, y: start, width: 1, height: abs(end - start))
        let entryRect = CGRect(x: 1, y: entry.start, width: 1, height: abs(entry.end - entry.start))
        return currentRect.intersects(entryRect)
    }

    // MARK: Setter

    func setGroup(_ group: Int) {
        self.group = group
    }

    func setColumn(_ col: Int) {
        self.col = col
    }

    func setHasDetailInfo(_ hasDetailInfo: Bool) {
        self.hasDetailInfo = hasDetailInfo
    }
}

final class TimelineTimeEntry: TimelineBaseTimeEntry {

    // MARK: Variables

    let timeEntry: TimeEntryViewItem
    let color: NSColor
    let name: String

    var isSmall: Bool {
        // It's small bar if duration less than 1 min
        return abs(timeEntry.duration_in_seconds) <= 60
    }

    // MARK: Init

    init(_ timeEntry: TimeEntryViewItem) {
        self.timeEntry = timeEntry
        self.name = timeEntry.descriptionName
        if let color = timeEntry.projectColor, !color.isEmpty {
            self.color = ConvertHexColor.hexCode(toNSColor: timeEntry.projectColor) ?? TimeEntryViewItem.defaultProjectColor()
        } else {
            self.color = TimeEntryViewItem.defaultProjectColor()
        }
        super.init(start: timeEntry.started.timeIntervalSince1970,
                   end: timeEntry.ended.timeIntervalSince1970)
    }

    // MARK: Public

    func isToday() -> Bool {
        guard let date = timeEntry.ended else { return false }
        return Calendar.current.isDateInToday(date)
    }

    func updateEndTimeForRunning() {
        end = Date().timeIntervalSince1970
    }
}
