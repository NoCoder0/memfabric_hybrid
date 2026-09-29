---
name: technical-design-proposal
description: Generate technical design proposals using a structured template. Use when user wants to create a technical design document, RFC, design proposal, or mentions "technical scheme", "design document", "proposal template".
---

# Technical Design Proposal Skill

Generate comprehensive technical design proposals using a structured template with guided sections.

## Quick Start

1. User provides context: feature description, problem statement, or requirements
2. Skill guides through each section of the template
3. Output a complete technical design document

## Workflow

### Phase 1: Gather Context

Start with the user's existing context, repository documentation, and relevant code to identify:

- Problem/feature description
- Target users and use cases
- Related Issue/PR links, when they exist; otherwise state that none is linked yet
- Existing constraints and requirements

Ask only for missing information that materially affects the scope or design and cannot be reliably inferred.
Continue independent drafting while awaiting answers. Label routine assumptions and unresolved decisions explicitly.

### Phase 2: Generate Document Sections

Follow the template structure, filling applicable sections and briefly marking others as not applicable.
Distinguish verified facts, proposed targets, and items awaiting validation. Never invent performance measurements,
hardware validation results, Issue/PR links, or settled decisions to complete the template.

#### 1. Overview (概述)

- **Summary**: 1-2 paragraphs on core objectives and value
- **Motivation**: Current pain points, user value, impact of not doing
- **Goals/Non-goals**: Clear boundaries of scope

#### 2. Use Case Analysis (用例分析)

- Functional requirements
- Performance indicators
- Security/privacy requirements
- DFX requirements (compatibility, maintainability, testability, reliability)

#### 3. Design (方案设计)

**3.1 Overall Design**

- Architecture diagram (if complex)
- Core logic flow
- Platform/OS/programming model choices
- Constraints and limitations

**3.2 Technical Alternatives**

- Considered but rejected approaches
- Trade-off analysis

**3.3 Functional & Performance Design**

- Implementation details
- Data model changes
- Impact scope

**3.4 Security & DFX Design**

- Security considerations
- Privacy handling
- DFX attributes

**3.5 Programming Interface Design** (if applicable)

- Development environment
- API definitions with parameter tables
- Programming manual outline

#### 4. Risks & Drawbacks (缺点和风险)

- Breaking changes
- Performance regression risks
- Migration path for existing users

#### 5. Prior Art (现有技术)

- Similar designs in other projects
- What to borrow vs. differ

#### 6. Open Issues (未解决问题)

- Decisions pending community input
- Must resolve before RFC approval

### Phase 3: Review & Refine

- [ ] Applicable sections completed; non-applicable sections identified
- [ ] Factual claims supported by code, documentation, or actual validation results
- [ ] Template placeholders replaced with concrete content or explicit unresolved items
- [ ] Performance targets distinguished from measured results, with a validation plan where needed
- [ ] Related Issue/PR linked when available; otherwise explicitly marked as not yet linked

## Template Reference

Use [template.md](template.md) for the Chinese document scaffold.
Read [REFERENCE.md](REFERENCE.md) when detailed section guidance is needed.

## Examples

Read [EXAMPLES.md](EXAMPLES.md) only when sample output would help; its scenarios and numbers are illustrative,
not evidence about this project.

## Tips

- Start with motivation and goals before diving into technical details
- Use diagrams for complex architectures (sequence, activity, state machine)
- Be explicit about non-goals to prevent scope creep
- Include specific metrics for performance requirements
- Provide code examples for API sections
