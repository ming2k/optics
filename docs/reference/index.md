# Reference Specifications

Technical descriptions of C APIs, exported symbols, material properties, effect operators, and concurrency rules. Consult these documents for exact lookup information.

---

## Specifications Directory

| Reference | Purpose & Scope |
| :--- | :--- |
| **[Public API Architecture](api.md)** | Core C API conventions, lifecycle contracts, and header structure. |
| **[Exported Symbol Index](symbols.md)** | Complete authoritative index of all exported C symbols, generated from public headers. |
| **[Prism Material System](prism.md)** | Technical specification for physical materials: Liquid Glass, Frosted Glass, Acrylic, and Mica. |
| **[Image Effect Operators](effect.md)** | Pixel operators, blur algorithms, tone mapping, and scratch render target rules in `flux_effect`. |
| **[Thread Safety & Concurrency](thread-safety.md)** | Thread-safety contracts across `flux_device`, surfaces, frame loops, and presentation. |
| **[Composition Graph](composition-graph.md)** | Historical reference on render passes, dependency tracking, and resource planning. |
| **[Glossary](glossary.md)** | Authoritative glossary of optics, rendering, geometry, and UI terminology. |

For guided walk-throughs, consult the [Tutorials](../tutorials/index.md).
For step-by-step problem-solving, consult the [How-To Guides](../how-to/index.md).
