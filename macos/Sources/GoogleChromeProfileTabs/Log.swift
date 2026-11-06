import os

/// The app's log in the system log: log show --last 10m --predicate 'subsystem == "com.max.google-chrome-profile-tabs"'
let log = Logger(subsystem: "com.max.google-chrome-profile-tabs", category: "app")
