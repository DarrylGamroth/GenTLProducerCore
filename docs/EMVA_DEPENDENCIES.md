# EMVA GenICam build inputs

The producer repositories do not require an installed complete GenICam
package. Builds use the smallest files published on the official
[EMVA downloads archive](https://www.emva.org/standards-technology/genicam/genicam-downloads-archive/).

| Consumer | Build input | Download size | Pinned SHA-256 |
| --- | --- | ---: | --- |
| All producers and `GenTLProducerCore` | [GenTL 1.6 reference header archive](https://www.emva.org/wp-content/uploads/GenTL_v1_6.zip) | 11,781 bytes | `48c0867219c4d04a968a27e740d81068fae08b6ab94c9834b221530d941bb4d1` |
| `continuousSDK3GenTL` | [PFNC reference header](https://www.emva.org/wp-content/uploads/PFNC.h) | 109,390 bytes | `6d9dd799df936e2c3c9646ecc49db13418428c7af582f928daa4d9e1b0d85e7e` |
| `continuousSDK3GenTL` schema test | [GenApi 1.1 schema archive](https://www.emva.org/wp-content/uploads/GenApiSchema_Version_1_1.xsd_.zip) | 6,811 bytes | `6dc9e3dd8b235a89b34423db3cf2bc10fd4f1ffd600bd3eb453a20858ce9ea9b` |

The extracted files are also verified. A hash mismatch stops configuration so
an upstream replacement cannot silently change a build. Explicit local paths
and download-disable options support offline and reproducible builds.

`continuousSDK3GenTL` uses GenApi-C only for its enhanced simulator conformance test.
That test is enabled when a GenApi-C runtime and headers are supplied. The
producer and its XML schema test do not require the roughly 121 MB standalone
GenApi reference-implementation download.

EMVA distributes the CLProtocol SDK headers in its GenICam package.
The CLProtocol GenTL build and the triggered CI workflow fetch that package and
extract the headers. The Core build uses the standalone GenTL archive.

## License handling

The current [GenICam license](https://www.emva.org/wp-content/uploads/GenICam_License_20231009.pdf)
treats headers delivered with the runtime reference implementation as source
under its modified BSD terms. Keep those files unmodified. A source
redistribution must retain the notices and disclaimer; a binary redistribution
must reproduce them in its documentation or accompanying materials. The
GenICam group and contributor names cannot be used as product endorsements
without prior written permission.

The PFNC header separately says that it may be used and modified without
restriction, while retaining its copyright and warranty text is prudent.
GenApi-C runtime redistribution has the same EMVA notice obligations plus any
licenses for third-party modules included in the runtime.

Downloading dependencies during configuration means these repositories do not
republish EMVA archives or headers. It does not remove the obligations of a
later source or binary distributor. Before distributing a CTI, prepare a
third-party-notice file from the unmodified downloaded header/license package
and review the separate continuous or triggered vendor SDK agreement. Those proprietary
SDK agreements are not present in these repositories, so their binary
redistribution rights are not established here.

Use a `GenICam TL compliant` claim only after verifying both GenTL and GenTL
SFNC behavior under EMVA's self-certification rules. This note is an engineering
summary, not legal advice.
