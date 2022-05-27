# 9.0.0
- [added] **Breaking change:** `FirebaseFirestoreSwift` has exited beta and is
  now generally available for use.

# 8.13.0
- [added] Added support for explicit typing to `DocumentReference.getDocument(as:)`
  and `DocumentSnapshot.data(as:)` to simplify mapping documents (#9101).
- [changed] `DocumentSnapshot.data(as:)` will decode the document to the type
  provided. If you expect that a document might *not exist*, use an optional
  type (e.g. `Book?.self`) to account for this. See
  [the documentation](https://firebase.google.com/docs/firestore/query-data/get-data#custom_objects)
  and this [blog post](https://peterfriese.dev/posts/firestore-codable-the-comprehensive-guide/#mapping-simple-types-using-codable)
  for an in-depth discussion.

# 8.12.1
- [added] Added async wrapper for `CollectionReference.addDocument()` and
  `Firestore.loadBundle()`.

# 8.9.0
- [added] Added `@FirestoreQuery` property wrapper for querying data from a
  Firestore collection.
- [changed] FirebaseFirestoreSwift now requires a minimum iOS version of 11 for
  all distributions.

# 7.7.0
- [feature] Added support for specifying `ServerTimestampBehavior` when
  decoding a `DocumentSnapshot`.

# 0.4
- [feature] Added conditional conformance to the `Hashable` protocol for the
  `@DocumentID`, `@ExplicitNull`, and `@ServerTimestamp` property wrappers.

- [fixed] Removed support for wrapping `NSDate` in a `@ServerTimestamp`
  property wrapper. This never actually worked because `NSDate` is not
  `Codable`.
- [fixed] Fixed the minimum supported Swift version to be 4.1. This was already
  effectively the case because the code made use of Swift 4.1 features without
  documenting this requirement.

# 0.3
- [fixed] Renamed the misspelled `FirestoreDecodingError.fieldNameConfict` to
  `fieldNameConflict` (#5520).

# 0.2
- Initial public release.
