# swiftcov on Kubernetes

Run PHPUnit path coverage inside a cluster using the prebuilt swiftcov image.
Because the extension is baked into the image (and registers at runtime as
`xdebug`), there is no init/sidecar and no compiler step — the container just
runs `phpunit --path-coverage`.

## Manifests

- [`coverage-job.yaml`](coverage-job.yaml) — a `batch/v1` `Job` that runs the
  suite with path coverage and writes Cobertura to `/app/build/cobertura.xml`,
  plus a minimal `PersistentVolumeClaim` (`swiftcov-project`) mounted at `/app`.

## Validate (no cluster needed)

```sh
kubectl apply --dry-run=client -f deploy/k8s/coverage-job.yaml
```

This checks the manifests against the client-side schema. It does not contact a
cluster.

## Apply (needs a real cluster)

```sh
# 1. Get your project code onto the PVC (CI checkout, initContainer, etc.).
# 2. Point the Job image tag at your PHP version (php8.2 / php8.3 / php8.4).
kubectl apply -f deploy/k8s/coverage-job.yaml
kubectl wait --for=condition=complete job/swiftcov-coverage --timeout=600s
kubectl logs -l app.kubernetes.io/name=swiftcov
```

Retrieve `build/cobertura.xml` from the PVC (or have the Job push it to your
artifact store) and feed it to your coverage dashboard unchanged.

## Notes

- The image tag must match your app's PHP version so the extension ABI lines up.
  For production / supply-chain integrity, pin the image by digest once it is
  published (`juslintek/swiftcov@sha256:<digest>`); the manifest uses a mutable
  tag for readability only.
- The Job is hardened: it runs as a non-root user (1000:1000), drops all Linux
  capabilities, forbids privilege escalation, and uses a read-only root
  filesystem. Writable `emptyDir` volumes are mounted at `/tmp` and `/app/build`
  (where the Cobertura report is written); the project PVC at `/app` is writable.
- `XDEBUG_MODE=coverage` is set explicitly; the image already defaults to it.
- Tune `resources` for large suites — path enumeration is the main memory sink,
  and swiftcov's memory guard degrades gracefully under pressure rather than
  OOM-ing (see the main README's "Memory management").
