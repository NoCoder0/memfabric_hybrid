# Examples

## Example 1: User Authentication System Redesign

**Authors:** @security-team
**Created:** 2024-01-15
**Updated:** 2024-01-20
**Related Issue/PR:** #456

---

# 1. Overview

## 1.1 Summary

This proposal outlines a comprehensive redesign of the user authentication system to support multi-factor authentication (MFA), OAuth 2.0 integration, and improved session management. The core value lies in enhancing security posture while maintaining a seamless user experience.

## 1.2 Motivation

Current authentication relies solely on username/password, which has led to:
- 23% of accounts compromised in the past year
- User complaints about password reset friction
- Compliance requirements for SOC 2 certification requiring MFA

Without this change, we risk regulatory non-compliance and continued security vulnerabilities.

## 1.3 Goals

**Goals:**
- Implement MFA with TOTP and SMS options
- Add OAuth 2.0 support for Google, GitHub, Microsoft
- Improve session security with refresh token rotation
- Achieve SOC 2 Type II compliance

**Non-goals:**
- Biometric authentication (Phase 2)
- Passwordless authentication (Phase 2)
- Legacy API migration (separate RFC)

---

# 2. Use Case Analysis

| Use Case | Priority | Performance Target |
|----------|----------|-------------------|
| Login with MFA | P0 | < 2s end-to-end |
| OAuth login | P0 | < 3s redirect flow |
| Token refresh | P1 | < 500ms |
| Session revocation | P1 | < 1s propagation |

**Security Requirements:**
- All tokens signed with RS256
- Refresh tokens hashed before storage
- MFA codes expire in 30 seconds

---

# 3. Design

## 3.1 Overall Design

```
┌─────────────┐     ┌──────────────┐     ┌─────────────┐
│   Client    │────▶│ Auth Service │────▶│   User DB   │
└─────────────┘     └──────────────┘     └─────────────┘
                           │
                           ▼
                    ┌──────────────┐
                    │  MFA Service │
                    └──────────────┘
```

Core flow:
1. User submits credentials
2. Auth service validates
3. If MFA enabled, send code via configured method
4. User verifies MFA code
5. Issue access + refresh tokens

## 3.2 Technical Alternatives

| Approach | Pros | Cons | Decision |
|----------|------|------|----------|
| JWT only | Stateless, simple | Cannot revoke instantly | Rejected |
| Session + JWT hybrid | Balanced | Complex | Selected |
| External IdP (Auth0) | Fast to market | Vendor lock-in, cost | Rejected |

## 3.5.2 Interface Definition

### 3.5.2.1 POST /auth/login

**Description:** Authenticate user with credentials

**Prototype:**
```typescript
POST /auth/login
Content-Type: application/json

{
  "email": string,
  "password": string,
  "mfa_code"?: string
}
```

**Input Parameters:**

| Parameter | Input/Output | Type | Description | Range |
|-----------|--------------|------|-------------|-------|
| email | Input | string | User email | Valid email |
| password | Input | string | User password | 8-128 chars |
| mfa_code | Input | string | MFA verification code | 6 digits |

**Return Parameters:**

| Parameter | Type | Description | Range |
|-----------|------|-------------|-------|
| access_token | string | JWT access token | - |
| refresh_token | string | Refresh token | - |
| expires_in | number | Token expiry in seconds | 3600 |

---

# 4. Risks & Drawbacks

- **Breaking Change:** Existing sessions will be invalidated on deployment
- **Migration:** Users must re-enroll MFA devices
- **Cost:** SMS MFA incurs ~$0.05/message via Twilio

**Mitigation:**
- Graceful migration window of 7 days
- In-app MFA enrollment prompts
- Budget allocation of $2000/month for SMS

---

# 5. Prior Art

- **Auth0:** OAuth 2.0 flow patterns
- **Okta:** MFA enrollment UX
- **Stripe:** Session management approach

---

# 6. Open Issues

- [ ] Decide on default MFA method (TOTP vs SMS)
- [ ] Determine token rotation frequency
- [ ] Approve budget for SMS provider
