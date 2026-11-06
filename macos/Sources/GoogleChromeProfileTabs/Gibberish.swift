/// Gibberish in place of a name: same length, spaces and case, alternating consonants and vowels so it looks
/// like a word rather than a gap. The same name with the same seed always gives the same gibberish.
enum Gibberish {
    private static let consonants = Array("bcdfghjklmnprstvz")
    private static let vowels = Array("aeiou")

    static func make(for text: String, seed: Int) -> String {
        var state = UInt64(bitPattern: Int64(seed)) ^ text.unicodeScalars.reduce(UInt64(14_695_981_039_346_656_037)) {
            ($0 ^ UInt64($1.value)) &* 1_099_511_628_211
        }
        state |= 1
        func next() -> UInt64 {
            state ^= state << 13
            state ^= state >> 7
            state ^= state << 17
            return state
        }
        var vowel = next() % 2 == 0
        return String(text.map { character -> Character in
            if character.isNumber {
                return Character(String(next() % 10))
            }
            guard character.isLetter else { return character }
            let pool = vowel ? vowels : consonants
            vowel.toggle()
            let letter = pool[Int(next() % UInt64(pool.count))]
            return character.isUppercase ? Character(letter.uppercased()) : letter
        })
    }
}
