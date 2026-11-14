USE bank;

INSERT INTO customers (name, email) VALUES
    ('Seed Customer 01', 'bank-seed-01@example.test'),
    ('Seed Customer 02', 'bank-seed-02@example.test'),
    ('Seed Customer 03', 'bank-seed-03@example.test'),
    ('Seed Customer 04', 'bank-seed-04@example.test'),
    ('Seed Customer 05', 'bank-seed-05@example.test'),
    ('Seed Customer 06', 'bank-seed-06@example.test'),
    ('Seed Customer 07', 'bank-seed-07@example.test'),
    ('Seed Customer 08', 'bank-seed-08@example.test'),
    ('Seed Customer 09', 'bank-seed-09@example.test'),
    ('Seed Customer 10', 'bank-seed-10@example.test'),
    ('Seed Customer 11', 'bank-seed-11@example.test'),
    ('Seed Customer 12', 'bank-seed-12@example.test'),
    ('Seed Customer 13', 'bank-seed-13@example.test'),
    ('Seed Customer 14', 'bank-seed-14@example.test'),
    ('Seed Customer 15', 'bank-seed-15@example.test'),
    ('Seed Customer 16', 'bank-seed-16@example.test'),
    ('Seed Customer 17', 'bank-seed-17@example.test'),
    ('Seed Customer 18', 'bank-seed-18@example.test'),
    ('Seed Customer 19', 'bank-seed-19@example.test'),
    ('Seed Customer 20', 'bank-seed-20@example.test'),
    ('Seed Customer 21', 'bank-seed-21@example.test'),
    ('Seed Customer 22', 'bank-seed-22@example.test'),
    ('Seed Customer 23', 'bank-seed-23@example.test'),
    ('Seed Customer 24', 'bank-seed-24@example.test'),
    ('Seed Customer 25', 'bank-seed-25@example.test'),
    ('Seed Customer 26', 'bank-seed-26@example.test'),
    ('Seed Customer 27', 'bank-seed-27@example.test'),
    ('Seed Customer 28', 'bank-seed-28@example.test'),
    ('Seed Customer 29', 'bank-seed-29@example.test'),
    ('Seed Customer 30', 'bank-seed-30@example.test'),
    ('Seed Customer 31', 'bank-seed-31@example.test'),
    ('Seed Customer 32', 'bank-seed-32@example.test'),
    ('Seed Customer 33', 'bank-seed-33@example.test'),
    ('Seed Customer 34', 'bank-seed-34@example.test'),
    ('Seed Customer 35', 'bank-seed-35@example.test'),
    ('Seed Customer 36', 'bank-seed-36@example.test'),
    ('Seed Customer 37', 'bank-seed-37@example.test'),
    ('Seed Customer 38', 'bank-seed-38@example.test'),
    ('Seed Customer 39', 'bank-seed-39@example.test'),
    ('Seed Customer 40', 'bank-seed-40@example.test'),
    ('Seed Customer 41', 'bank-seed-41@example.test'),
    ('Seed Customer 42', 'bank-seed-42@example.test'),
    ('Seed Customer 43', 'bank-seed-43@example.test'),
    ('Seed Customer 44', 'bank-seed-44@example.test'),
    ('Seed Customer 45', 'bank-seed-45@example.test'),
    ('Seed Customer 46', 'bank-seed-46@example.test'),
    ('Seed Customer 47', 'bank-seed-47@example.test'),
    ('Seed Customer 48', 'bank-seed-48@example.test'),
    ('Seed Customer 49', 'bank-seed-49@example.test'),
    ('Seed Customer 50', 'bank-seed-50@example.test')
ON DUPLICATE KEY UPDATE name = VALUES(name);

INSERT INTO accounts (customer_id, balance)
SELECT customer_id, 100000.00
FROM customers
WHERE email LIKE 'bank-seed-%@example.test'
  AND NOT EXISTS (
      SELECT 1
      FROM accounts existing
      WHERE existing.customer_id = customers.customer_id
  );
